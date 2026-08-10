// Arcade button bench test for a spare ESP32-S3 DevKitC-1. Validates the
// switches and their 5V lamps before they are committed to the court unit and
// the remotes, and measures the contact bounce that the product debounce
// constants assume (RemoteCoreConfig::stable_press_ms in
// components/remote_core, WiredButton in firmware/court-display).
//
// Edges are timestamped in an ISR, so a bounce burst resolves to microseconds
// instead of to the 5-10 ms poll interval the product firmware runs at. A run
// of edges counts as one transition once the pin has been quiet for the
// settle window; the level is re-read at that point so the ISR never decides
// the outcome.

#include <cinttypes>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {

constexpr const char* TAG = "buttontest";

constexpr int kMaxButtons = 5;
constexpr int kButtonCount = CONFIG_BUTTON_TEST_COUNT;

constexpr int kButtonGpio[kMaxButtons] = {
    CONFIG_BUTTON_TEST_GPIO_1, CONFIG_BUTTON_TEST_GPIO_2, CONFIG_BUTTON_TEST_GPIO_3,
    CONFIG_BUTTON_TEST_GPIO_4, CONFIG_BUTTON_TEST_GPIO_5};

constexpr int kLampGpio[kMaxButtons] = {
    CONFIG_BUTTON_TEST_LAMP_1, CONFIG_BUTTON_TEST_LAMP_2, CONFIG_BUTTON_TEST_LAMP_3,
    CONFIG_BUTTON_TEST_LAMP_4, CONFIG_BUTTON_TEST_LAMP_5};

#if CONFIG_BUTTON_TEST_LAMP_ACTIVE_HIGH
constexpr bool kLampActiveHigh = true;
#else
constexpr bool kLampActiveHigh = false;
#endif

constexpr std::int64_t kSettleUs = CONFIG_BUTTON_TEST_SETTLE_MS * 1000LL;
constexpr std::int64_t kBounceWarnUs = CONFIG_BUTTON_TEST_BOUNCE_WARN_MS * 1000LL;

// BOOT button: tap prints a summary, hold 2 s clears the counters.
constexpr gpio_num_t kBootButton = GPIO_NUM_0;
constexpr std::int64_t kBootDebounceUs = 50'000;
constexpr std::int64_t kResetHoldUs = 2'000'000;

constexpr std::int64_t kSummaryIntervalUs = 10'000'000;

struct Edge {
    std::uint8_t index;
    std::int64_t at_us;
};

struct ButtonStats {
    bool stable_pressed;
    bool burst_active;
    std::int64_t burst_start_us;
    std::int64_t last_edge_us;
    std::uint32_t burst_edges;

    std::uint32_t presses;
    std::uint32_t releases;
    std::uint32_t glitches;
    std::uint32_t max_burst_edges;
    std::uint32_t max_bounce_us;
    std::uint64_t total_bounce_us;
    std::uint32_t bounce_samples;
    std::int64_t press_started_us;
    std::uint32_t min_hold_ms;
    std::uint32_t max_hold_ms;
};

ButtonStats s_stats[kMaxButtons] = {};
QueueHandle_t s_edges = nullptr;
volatile std::uint32_t s_edges_dropped = 0;
bool s_activity_since_summary = false;

void IRAM_ATTR edge_isr(void* arg) {
    const Edge edge{static_cast<std::uint8_t>(reinterpret_cast<std::uintptr_t>(arg)),
                    esp_timer_get_time()};
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_edges, &edge, &woken) != pdTRUE) {
        s_edges_dropped = s_edges_dropped + 1;
    }
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void set_lamp(int index, bool on) {
    if (kLampGpio[index] < 0) {
        return;
    }
    gpio_set_level(static_cast<gpio_num_t>(kLampGpio[index]), on == kLampActiveHigh ? 1 : 0);
}

void init_gpio() {
    std::uint64_t inputs = 0;
    std::uint64_t outputs = 0;
    for (int i = 0; i < kButtonCount; ++i) {
        inputs |= 1ULL << kButtonGpio[i];
        if (kLampGpio[i] >= 0) {
            outputs |= 1ULL << kLampGpio[i];
        }
    }

    gpio_config_t in{};
    in.pin_bit_mask = inputs;
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    in.intr_type = GPIO_INTR_ANYEDGE;
    ESP_ERROR_CHECK(gpio_config(&in));

    gpio_config_t boot{};
    boot.pin_bit_mask = 1ULL << kBootButton;
    boot.mode = GPIO_MODE_INPUT;
    boot.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&boot));

    if (outputs != 0) {
        gpio_config_t out{};
        out.pin_bit_mask = outputs;
        out.mode = GPIO_MODE_OUTPUT;
        ESP_ERROR_CHECK(gpio_config(&out));
    }
    for (int i = 0; i < kButtonCount; ++i) {
        set_lamp(i, false);
    }

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    for (int i = 0; i < kButtonCount; ++i) {
        ESP_ERROR_CHECK(gpio_isr_handler_add(static_cast<gpio_num_t>(kButtonGpio[i]), edge_isr,
                                             reinterpret_cast<void*>(
                                                 static_cast<std::uintptr_t>(i))));
    }
}

// Each lamp on in turn: proves the lamps, their drivers and the wiring order
// in one glance before any button is touched.
void lamp_chase() {
    for (int i = 0; i < kButtonCount; ++i) {
        set_lamp(i, true);
        vTaskDelay(pdMS_TO_TICKS(300));
        set_lamp(i, false);
    }
}

void record_edge(const Edge& edge) {
    if (edge.index >= kButtonCount) {
        return;
    }
    ButtonStats& b = s_stats[edge.index];
    if (!b.burst_active) {
        b.burst_active = true;
        b.burst_start_us = edge.at_us;
        b.burst_edges = 0;
    }
    ++b.burst_edges;
    b.last_edge_us = edge.at_us;
}

void settle_button(int index, std::int64_t now) {
    ButtonStats& b = s_stats[index];
    if (!b.burst_active || now - b.last_edge_us < kSettleUs) {
        return;
    }
    b.burst_active = false;

    const int gpio = kButtonGpio[index];
    const auto bounce_us = static_cast<std::uint32_t>(b.last_edge_us - b.burst_start_us);
    const bool pressed = gpio_get_level(static_cast<gpio_num_t>(gpio)) == 0;

    if (b.burst_edges > b.max_burst_edges) {
        b.max_burst_edges = b.burst_edges;
    }
    if (bounce_us > b.max_bounce_us) {
        b.max_bounce_us = bounce_us;
    }
    b.total_bounce_us += bounce_us;
    ++b.bounce_samples;
    s_activity_since_summary = true;

    // Edges that resolve back to the level we were already at: a contact
    // glitch. Harmless here, but on the remote it would burn a retrigger
    // guard window, so they are counted separately.
    if (pressed == b.stable_pressed) {
        ++b.glitches;
        ESP_LOGW(TAG,
                 "button %d (GPIO%d): glitch, %" PRIu32 " edges over %" PRIu32
                 " us, still %s",
                 index + 1, gpio, b.burst_edges, bounce_us, pressed ? "pressed" : "released");
        return;
    }

    b.stable_pressed = pressed;
    set_lamp(index, pressed);

    if (pressed) {
        ++b.presses;
        b.press_started_us = b.burst_start_us;
        ESP_LOGI(TAG,
                 "button %d (GPIO%d): press #%" PRIu32 ", bounce %" PRIu32 " us over %" PRIu32
                 " edges",
                 index + 1, gpio, b.presses, bounce_us, b.burst_edges);
    } else {
        ++b.releases;
        const auto hold_ms =
            static_cast<std::uint32_t>((b.burst_start_us - b.press_started_us) / 1000);
        if (b.min_hold_ms == 0 || hold_ms < b.min_hold_ms) {
            b.min_hold_ms = hold_ms;
        }
        if (hold_ms > b.max_hold_ms) {
            b.max_hold_ms = hold_ms;
        }
        ESP_LOGI(TAG,
                 "button %d (GPIO%d): release after %" PRIu32 " ms, bounce %" PRIu32
                 " us over %" PRIu32 " edges",
                 index + 1, gpio, hold_ms, bounce_us, b.burst_edges);
    }

    if (bounce_us >= kBounceWarnUs) {
        ESP_LOGW(TAG,
                 "button %d (GPIO%d): bounce %" PRIu32 " us reaches the %d ms debounce the "
                 "firmware assumes",
                 index + 1, gpio, bounce_us, CONFIG_BUTTON_TEST_BOUNCE_WARN_MS);
    }
}

void print_summary(std::int64_t now) {
    ESP_LOGI(TAG, "=== summary at %" PRIu32 " s ===", static_cast<std::uint32_t>(now / 1000000));
    ESP_LOGI(TAG, " btn gpio  press  relse glitch  maxBounce  avgBounce  edges  minHold  maxHold");

    std::uint32_t worst_bounce_us = 0;
    std::uint32_t total_glitches = 0;
    for (int i = 0; i < kButtonCount; ++i) {
        const ButtonStats& b = s_stats[i];
        const auto avg_bounce_us =
            b.bounce_samples == 0
                ? 0u
                : static_cast<std::uint32_t>(b.total_bounce_us / b.bounce_samples);
        ESP_LOGI(TAG,
                 " %3d %4d %6" PRIu32 " %6" PRIu32 " %6" PRIu32 " %7" PRIu32 " us %7" PRIu32
                 " us %6" PRIu32 " %6" PRIu32 " %6" PRIu32,
                 i + 1, kButtonGpio[i], b.presses, b.releases, b.glitches, b.max_bounce_us,
                 avg_bounce_us, b.max_burst_edges, b.min_hold_ms, b.max_hold_ms);
        if (b.max_bounce_us > worst_bounce_us) {
            worst_bounce_us = b.max_bounce_us;
        }
        total_glitches += b.glitches;
    }

    if (s_edges_dropped != 0) {
        ESP_LOGW(TAG, "edge queue overflowed: %" PRIu32 " events lost, numbers are a lower bound",
                 s_edges_dropped);
    }
    if (worst_bounce_us >= kBounceWarnUs) {
        ESP_LOGW(TAG,
                 "verdict: worst bounce %" PRIu32 " us >= %d ms - raise stable_press_ms in "
                 "components/remote_core and kWiredButtonPressMs in court-display",
                 worst_bounce_us, CONFIG_BUTTON_TEST_BOUNCE_WARN_MS);
    } else {
        ESP_LOGI(TAG,
                 "verdict: worst bounce %" PRIu32 " us, %" PRIu32
                 " glitches - the %d ms firmware debounce holds",
                 worst_bounce_us, total_glitches, CONFIG_BUTTON_TEST_BOUNCE_WARN_MS);
    }
}

void reset_stats() {
    for (int i = 0; i < kButtonCount; ++i) {
        const bool pressed = s_stats[i].stable_pressed;
        s_stats[i] = ButtonStats{};
        s_stats[i].stable_pressed = pressed;
    }
    s_edges_dropped = 0;
    s_activity_since_summary = false;
    ESP_LOGI(TAG, "counters cleared");
}

}  // namespace

extern "C" void app_main(void) {
    s_edges = xQueueCreate(256, sizeof(Edge));
    ESP_ERROR_CHECK(s_edges == nullptr ? ESP_ERR_NO_MEM : ESP_OK);

    init_gpio();

    ESP_LOGI(TAG, "arcade button test: %d button(s), settle %d ms, bounce warning at %d ms",
             kButtonCount, CONFIG_BUTTON_TEST_SETTLE_MS, CONFIG_BUTTON_TEST_BOUNCE_WARN_MS);
    for (int i = 0; i < kButtonCount; ++i) {
        ESP_LOGI(TAG, "  button %d: switch GPIO%d (active low), lamp GPIO%d", i + 1,
                 kButtonGpio[i], kLampGpio[i]);
    }
    ESP_LOGI(TAG, "BOOT: tap = summary, hold 2 s = clear counters");

    lamp_chase();

    // A button already down at boot would otherwise look like a press later.
    for (int i = 0; i < kButtonCount; ++i) {
        s_stats[i].stable_pressed = gpio_get_level(static_cast<gpio_num_t>(kButtonGpio[i])) == 0;
        set_lamp(i, s_stats[i].stable_pressed);
        if (s_stats[i].stable_pressed) {
            ESP_LOGW(TAG, "button %d (GPIO%d) reads pressed at boot - check the wiring", i + 1,
                     kButtonGpio[i]);
        }
    }

    // Anything touched during the chase is already queued; the snapshot above
    // is the truth.
    xQueueReset(s_edges);

    std::int64_t last_summary_us = esp_timer_get_time();
    bool boot_down = false;
    std::int64_t boot_since_us = 0;

    Edge edge;
    while (true) {
        while (xQueueReceive(s_edges, &edge, pdMS_TO_TICKS(5)) == pdTRUE) {
            record_edge(edge);
        }

        const std::int64_t now = esp_timer_get_time();
        for (int i = 0; i < kButtonCount; ++i) {
            settle_button(i, now);
        }

        const bool down = gpio_get_level(kBootButton) == 0;
        if (down && !boot_down) {
            boot_down = true;
            boot_since_us = now;
        } else if (!down && boot_down) {
            boot_down = false;
            const std::int64_t held = now - boot_since_us;
            if (held >= kResetHoldUs) {
                reset_stats();
            } else if (held >= kBootDebounceUs) {
                print_summary(now);
                s_activity_since_summary = false;
                last_summary_us = now;
            }
        }

        if (now - last_summary_us >= kSummaryIntervalUs) {
            last_summary_us = now;
            if (s_activity_since_summary) {
                print_summary(now);
                s_activity_since_summary = false;
            }
        }
    }
}
