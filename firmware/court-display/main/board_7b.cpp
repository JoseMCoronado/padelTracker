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

// IO EXTENSION (CH32V003) lines (EXIOx), per the 7B wiki pin table.
constexpr uint8_t kExioTouchReset = 1 << 1;  // TP_RST
constexpr uint8_t kExioBacklight = 1 << 2;   // DISP (backlight enable)
constexpr uint8_t kExioLcdReset = 1 << 3;    // LCD_RST - panel shows backlight-only
                                             // white when held low (not in the wiki
                                             // pin table, confirmed on hardware)
constexpr uint8_t kExioSdCs = 1 << 4;        // TF card CS, active low - keep high
constexpr uint8_t kExioUsbSel = 1 << 5;      // low = USB mode, high = CAN
constexpr uint8_t kExioLcdVddEn = 1 << 6;    // LCD_VDD_EN (panel VCOM power)

// Timings straight from Waveshare's 7B demo (rgb_lcd_port.h). PCLK must be
// 30 MHz: 16 MHz gave a ~17 Hz refresh, slow enough for a faint whole-panel
// inversion flicker (seen on hardware).
constexpr uint32_t kPclkHz = 30 * 1000 * 1000;
constexpr uint32_t kHsyncPulse = 162;
constexpr uint32_t kHsyncBackPorch = 152;
constexpr uint32_t kHsyncFrontPorch = 48;
constexpr uint32_t kVsyncPulse = 45;
constexpr uint32_t kVsyncBackPorch = 13;
constexpr uint32_t kVsyncFrontPorch = 3;

// --- IO EXTENSION (CH32V003) ------------------------------------------------
// The 7B's "IO EXTENSION" is a CH32V003 microcontroller at address 0x24
// speaking a normal register protocol (NOT the CH422G used on older Waveshare
// boards, whose registers live at distinct I2C addresses):
//   0x02 direction (1 = output), 0x03 output levels, 0x04 input readback,
//   0x05 backlight PWM duty (0-255, keep <= 247), 0x06 ADC.
// The duty register is volatile: on a cold boot the backlight stays dark
// until it is written, even with DISP (EXIO2) high.

constexpr uint8_t kIoExtAddress = 0x24;
constexpr uint8_t kIoExtRegDirection = 0x02;
constexpr uint8_t kIoExtRegOutput = 0x03;
constexpr uint8_t kIoExtRegInput = 0x04;
constexpr uint8_t kIoExtRegPwm = 0x05;
// NOTE: do not write the PWM register (0x05). Waveshare's LCD demo never
// touches it and the backlight runs steady at full brightness; writing it
// engages the chip's slow PWM mode, which flickers visibly (observed on
// hardware with duty 128 and 0 alike, until a full power cycle).

i2c_master_bus_handle_t s_i2c_bus = nullptr;
i2c_master_dev_handle_t s_ioext = nullptr;
uint8_t s_exio_state = 0;

bool ioext_write(uint8_t reg, uint8_t value) {
    const uint8_t frame[2] = {reg, value};
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3 && err != ESP_OK; ++attempt) {
        err = i2c_master_transmit(s_ioext, frame, sizeof(frame), 100);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    // The CH32V003 firmware needs a beat between transactions.
    vTaskDelay(pdMS_TO_TICKS(2));
    return err == ESP_OK;
}

bool ioext_read(uint8_t reg, uint8_t* out) {
    return i2c_master_transmit_receive(s_ioext, &reg, 1, out, 1, 100) == ESP_OK;
}

bool exio_apply() {
    return ioext_write(kIoExtRegOutput, s_exio_state);
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

    if (i2c_master_probe(s_i2c_bus, kIoExtAddress, 100) != ESP_OK) {
        ESP_LOGE(TAG, "IO extension (CH32V003) not responding at 0x24");
        return false;
    }
    i2c_device_config_t device_config{};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = kIoExtAddress;
    device_config.scl_speed_hz = 100 * 1000;
    if (i2c_master_bus_add_device(s_i2c_bus, &device_config, &s_ioext) != ESP_OK) {
        ESP_LOGE(TAG, "IO extension: add device failed");
        return false;
    }

    if (!ioext_write(kIoExtRegDirection, 0xFF)) {  // all EXIO pins as outputs
        ESP_LOGE(TAG, "IO extension: direction write failed");
        return false;
    }

    // Panel power on with the panel briefly held in reset; touch also held in
    // reset, USB_SEL low (USB mode), TF card deselected.
    s_exio_state = kExioLcdVddEn | kExioBacklight | kExioSdCs;
    if (!exio_apply()) {
        ESP_LOGE(TAG, "IO extension: output write failed (0x%02X)", s_exio_state);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    // Release the panel reset once its power rail is up.
    s_exio_state |= kExioLcdReset;
    if (!exio_apply()) {
        ESP_LOGE(TAG, "IO extension: LCD reset release failed (0x%02X)", s_exio_state);
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    // Drive INT low while releasing reset so the GT911 latches I2C address
    // 0x5D (floating INT at reset release can select 0x14 instead).
    gpio_config_t int_low{};
    int_low.pin_bit_mask = 1ULL << kPinTouchIrq;
    int_low.mode = GPIO_MODE_OUTPUT;
    gpio_config(&int_low);
    gpio_set_level(kPinTouchIrq, 0);

    s_exio_state |= kExioTouchReset;
    if (!exio_apply()) {
        ESP_LOGE(TAG, "IO extension: touch still held in reset (0x%02X)", s_exio_state);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    // Hand INT back to the touch driver (it reconfigures it as an input).
    gpio_reset_pin(kPinTouchIrq);
    vTaskDelay(pdMS_TO_TICKS(200));

    // Verify the expander outputs actually latched what we wrote.
    uint8_t io_state = 0;
    if (ioext_read(kIoExtRegInput, &io_state)) {
        ESP_LOGI(TAG, "IO extension readback: wrote 0x%02X, pins read 0x%02X %s",
                 s_exio_state, io_state, io_state == s_exio_state ? "(MATCH)" : "(MISMATCH)");
    } else {
        ESP_LOGE(TAG, "IO extension readback failed (reg 0x04)");
    }
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

bool init_touch_at(uint16_t dev_addr) {
    // Same values as ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG(); that macro's
    // designator order is not valid C++.
    esp_lcd_panel_io_i2c_config_t io_config{};
    io_config.dev_addr = dev_addr;
    io_config.scl_speed_hz = 100 * 1000;
    io_config.control_phase_bytes = 1;
    io_config.dc_bit_offset = 0;
    io_config.lcd_cmd_bits = 16;
    io_config.flags.disable_control_phase = 1;
    esp_lcd_panel_io_handle_t io = nullptr;
    if (esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_config, &io) != ESP_OK) {
        ESP_LOGE(TAG, "touch panel io failed (addr 0x%02X)", dev_addr);
        return false;
    }

    esp_lcd_touch_config_t touch_config{};
    touch_config.x_max = kHRes;
    touch_config.y_max = kVRes;
    touch_config.rst_gpio_num = GPIO_NUM_NC;  // reset is on the expander
    touch_config.int_gpio_num = kPinTouchIrq;
    if (esp_lcd_touch_new_i2c_gt911(io, &touch_config, &s_touch) != ESP_OK) {
        ESP_LOGE(TAG, "GT911 init failed at 0x%02X", dev_addr);
        esp_lcd_panel_io_del(io);
        return false;
    }
    ESP_LOGI(TAG, "GT911 up at 0x%02X", dev_addr);
    return true;
}

bool init_touch() {
    // The GT911 latches its I2C address from the INT level at reset release;
    // log which address actually answers, then try both.
    const uint16_t probe_addrs[] = {ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
                                    ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP};
    for (uint16_t addr : probe_addrs) {
        const bool acks = i2c_master_probe(s_i2c_bus, addr, 100) == ESP_OK;
        ESP_LOGI(TAG, "GT911 probe 0x%02X: %s", addr, acks ? "ACK" : "no ack");
    }
    return init_touch_at(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS) ||
           init_touch_at(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP);
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
