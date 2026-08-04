#include "board_7b.hpp"

#include <cstring>

#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace board {
namespace {

const char* TAG = "board7b";

// --- Wiki pinout (ESP32-S3-Touch-LCD-7B) -----------------------------------

constexpr gpio_num_t kPinHsync = GPIO_NUM_46;
constexpr gpio_num_t kPinVsync = GPIO_NUM_3;
constexpr gpio_num_t kPinDe = GPIO_NUM_5;
constexpr gpio_num_t kPinPclk = GPIO_NUM_7;

// RGB565 data lines, data0..data15 = B3..B7, G2..G7, R3..R7.
constexpr int kDataPins[16] = {
    14, 38, 18, 17, 10,      // B3 B4 B5 B6 B7
    39, 0, 45, 48, 47, 21,   // G2 G3 G4 G5 G6 G7
    1, 2, 42, 41, 40,        // R3 R4 R5 R6 R7
};

constexpr gpio_num_t kPinTouchSda = GPIO_NUM_8;
constexpr gpio_num_t kPinTouchScl = GPIO_NUM_9;
constexpr gpio_num_t kPinTouchIrq = GPIO_NUM_4;

// CH422G IO expander lines (EXIOx).
constexpr uint8_t kExioTouchReset = 1 << 1;  // TP_RST
constexpr uint8_t kExioBacklight = 1 << 2;   // DISP
constexpr uint8_t kExioLcdVddEn = 1 << 6;    // LCD_VDD_EN

// Community-confirmed timings for the 7B's 1024x600 panel.
constexpr uint32_t kPclkHz = 16 * 1000 * 1000;
constexpr uint32_t kHsyncPulse = 162;
constexpr uint32_t kHsyncBackPorch = 152;
constexpr uint32_t kHsyncFrontPorch = 48;
constexpr uint32_t kVsyncPulse = 45;
constexpr uint32_t kVsyncBackPorch = 13;
constexpr uint32_t kVsyncFrontPorch = 3;

// --- CH422G ---------------------------------------------------------------
// Minimal driver: the chip maps registers onto distinct I2C addresses.
// WR_SET (0x24) configures the chip (bit0 = IO_OE, push-pull outputs on
// IO0..7); WR_IO (0x38) sets the output byte.

constexpr uint8_t kCh422gRegWrSet = 0x24;
constexpr uint8_t kCh422gRegWrIo = 0x38;

i2c_master_bus_handle_t s_i2c_bus = nullptr;
uint8_t s_exio_state = 0;

bool ch422g_write(uint8_t reg_addr, uint8_t value) {
    i2c_device_config_t device_config{};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = reg_addr;
    device_config.scl_speed_hz = 400 * 1000;
    i2c_master_dev_handle_t device = nullptr;
    if (i2c_master_bus_add_device(s_i2c_bus, &device_config, &device) != ESP_OK) {
        return false;
    }
    const esp_err_t err = i2c_master_transmit(device, &value, 1, 100);
    i2c_master_bus_rm_device(device);
    return err == ESP_OK;
}

bool exio_apply() {
    return ch422g_write(kCh422gRegWrIo, s_exio_state);
}

// --- LVGL glue --------------------------------------------------------------

esp_lcd_panel_handle_t s_panel = nullptr;
esp_lcd_touch_handle_t s_touch = nullptr;
lv_disp_draw_buf_t s_draw_buf;
lv_disp_drv_t s_disp_drv;
lv_indev_drv_t s_indev_drv;

void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_map) {
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1,
                              color_map);
    lv_disp_flush_ready(drv);
}

void touch_read_cb(lv_indev_drv_t* /*drv*/, lv_indev_data_t* data) {
    esp_lcd_touch_read_data(s_touch);
    uint16_t x = 0;
    uint16_t y = 0;
    uint8_t count = 0;
    const bool pressed = esp_lcd_touch_get_coordinates(s_touch, &x, &y, nullptr, &count, 1);
    if (pressed && count > 0) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void tick_cb(void* /*arg*/) {
    lv_tick_inc(2);
}

bool init_expander_and_power() {
    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = kPinTouchSda;
    bus_config.scl_io_num = kPinTouchScl;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bus_config, &s_i2c_bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed");
        return false;
    }

    if (!ch422g_write(kCh422gRegWrSet, 0x01)) {  // IO_OE: push-pull outputs
        ESP_LOGE(TAG, "CH422G not responding");
        return false;
    }

    // Panel power + backlight on, touch controller held in reset...
    s_exio_state = kExioLcdVddEn | kExioBacklight;
    exio_apply();
    vTaskDelay(pdMS_TO_TICKS(100));
    // ...then released. INT is low during reset, selecting I2C address 0x5D.
    s_exio_state |= kExioTouchReset;
    exio_apply();
    vTaskDelay(pdMS_TO_TICKS(200));
    return true;
}

bool init_panel() {
    esp_lcd_rgb_panel_config_t config{};
    config.clk_src = LCD_CLK_SRC_DEFAULT;
    config.timings.pclk_hz = kPclkHz;
    config.timings.h_res = kHRes;
    config.timings.v_res = kVRes;
    config.timings.hsync_pulse_width = kHsyncPulse;
    config.timings.hsync_back_porch = kHsyncBackPorch;
    config.timings.hsync_front_porch = kHsyncFrontPorch;
    config.timings.vsync_pulse_width = kVsyncPulse;
    config.timings.vsync_back_porch = kVsyncBackPorch;
    config.timings.vsync_front_porch = kVsyncFrontPorch;
    config.timings.flags.pclk_active_neg = 1;
    config.data_width = 16;
    config.bits_per_pixel = 16;
    config.num_fbs = 1;
    config.bounce_buffer_size_px = static_cast<size_t>(kHRes) * 10;
    config.psram_trans_align = 64;
    config.hsync_gpio_num = kPinHsync;
    config.vsync_gpio_num = kPinVsync;
    config.de_gpio_num = kPinDe;
    config.pclk_gpio_num = kPinPclk;
    config.disp_gpio_num = -1;  // display enable is on the expander
    for (int i = 0; i < 16; ++i) {
        config.data_gpio_nums[i] = kDataPins[i];
    }
    config.flags.fb_in_psram = 1;

    if (esp_lcd_new_rgb_panel(&config, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "RGB panel create failed");
        return false;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    return true;
}

bool init_touch() {
    // Same values as ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG(); that macro's
    // designator order is not valid C++.
    esp_lcd_panel_io_i2c_config_t io_config{};
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    io_config.control_phase_bytes = 1;
    io_config.dc_bit_offset = 0;
    io_config.lcd_cmd_bits = 16;
    io_config.flags.disable_control_phase = 1;
    esp_lcd_panel_io_handle_t io = nullptr;
    if (esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_config, &io) != ESP_OK) {
        ESP_LOGE(TAG, "touch panel io failed");
        return false;
    }

    esp_lcd_touch_config_t touch_config{};
    touch_config.x_max = kHRes;
    touch_config.y_max = kVRes;
    touch_config.rst_gpio_num = GPIO_NUM_NC;  // reset is on the expander
    touch_config.int_gpio_num = kPinTouchIrq;
    if (esp_lcd_touch_new_i2c_gt911(io, &touch_config, &s_touch) != ESP_OK) {
        ESP_LOGE(TAG, "GT911 init failed");
        return false;
    }
    return true;
}

}  // namespace

bool init_display(void) {
    if (!init_expander_and_power() || !init_panel()) {
        return false;
    }
    const bool touch_ok = init_touch();
    if (!touch_ok) {
        // A dead touch controller degrades to display-only; remotes and
        // wired buttons still score. Surface it, don't halt.
        ESP_LOGE(TAG, "touch unavailable, continuing display-only");
    }

    lv_init();

    // Two partial draw buffers in PSRAM (1/10th of the screen each).
    const size_t buf_pixels = static_cast<size_t>(kHRes) * (kVRes / 10);
    auto* buf1 = static_cast<lv_color_t*>(
        heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM));
    auto* buf2 = static_cast<lv_color_t*>(
        heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM));
    if (buf1 == nullptr || buf2 == nullptr) {
        ESP_LOGE(TAG, "LVGL draw buffer alloc failed");
        return false;
    }
    lv_disp_draw_buf_init(&s_draw_buf, buf1, buf2, buf_pixels);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = kHRes;
    s_disp_drv.ver_res = kVRes;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&s_disp_drv);

    if (touch_ok) {
        lv_indev_drv_init(&s_indev_drv);
        s_indev_drv.type = LV_INDEV_TYPE_POINTER;
        s_indev_drv.read_cb = touch_read_cb;
        lv_indev_drv_register(&s_indev_drv);
    }

    const esp_timer_create_args_t tick_args = {
        .callback = tick_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lv_tick",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t tick_timer = nullptr;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 2000));

    ESP_LOGI(TAG, "display up: %dx%d, touch %s", kHRes, kVRes, touch_ok ? "ok" : "ABSENT");
    return true;
}

void set_backlight(bool on) {
    if (on) {
        s_exio_state |= kExioBacklight;
    } else {
        s_exio_state &= static_cast<uint8_t>(~kExioBacklight);
    }
    exio_apply();
}

}  // namespace board
