#include "buzzer.hpp"

#include <mutex>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"

namespace buzzer {
namespace {

using padel::sound::Pattern;
using padel::sound::Tone;

constexpr gpio_num_t kGpio = static_cast<gpio_num_t>(CONFIG_PADEL_COURT_BUZZER_GPIO);

#if CONFIG_PADEL_COURT_BUZZER_PASSIVE
constexpr ledc_mode_t kMode = LEDC_LOW_SPEED_MODE;  // the only mode on the S3
constexpr ledc_timer_t kTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kChannel = LEDC_CHANNEL_0;
// 10 bits still resolves an exact 50% square at 5 kHz, which is all a buzzer
// needs; more resolution would only cost usable frequency range.
constexpr ledc_timer_bit_t kDutyBits = LEDC_TIMER_10_BIT;
constexpr std::uint32_t kDutyHalf = 512;
#endif

esp_timer_handle_t s_step_timer = nullptr;
std::mutex s_mutex;
Pattern s_pattern{};
std::size_t s_index = 0;

void output(std::uint16_t freq_hz) {
#if CONFIG_PADEL_COURT_BUZZER_PASSIVE
    if (freq_hz == 0) {
        ledc_set_duty(kMode, kChannel, 0);
        ledc_update_duty(kMode, kChannel);
        return;
    }
    ledc_set_freq(kMode, kTimer, freq_hz);
    ledc_set_duty(kMode, kChannel, kDutyHalf);
    ledc_update_duty(kMode, kChannel);
#else
    // An active buzzer sounds at its own pitch, so only the rhythm survives:
    // the rests between notes still make each cue count differently.
    gpio_set_level(kGpio, freq_hz != 0 ? 1 : 0);
#endif
}

// Sounds the step at s_index and schedules the one after it. Caller holds
// s_mutex.
void advance_locked() {
    if (s_index >= s_pattern.count) {
        output(0);
        return;
    }
    const Tone tone = s_pattern.tones[s_index++];
    output(tone.freq_hz);
    esp_timer_start_once(s_step_timer, static_cast<std::uint64_t>(tone.duration_ms) * 1000);
}

void step_cb(void* /*arg*/) {
    std::lock_guard<std::mutex> lock(s_mutex);
    // A play() that landed between this timer firing and this lock being taken
    // has already re-armed the timer for its own first step. Advancing here
    // too would cut that step short, so a live timer means this callback
    // belongs to the pattern that was just replaced.
    if (esp_timer_is_active(s_step_timer)) {
        return;
    }
    advance_locked();
}

}  // namespace

void init() {
#if CONFIG_PADEL_COURT_BUZZER_PASSIVE
    ledc_timer_config_t timer{};
    timer.speed_mode = kMode;
    timer.duty_resolution = kDutyBits;
    timer.timer_num = kTimer;
    timer.freq_hz = padel::sound::kMinToneHz;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel{};
    channel.gpio_num = kGpio;
    channel.speed_mode = kMode;
    channel.channel = kChannel;
    channel.timer_sel = kTimer;
    channel.duty = 0;  // silent until a cue plays
    channel.hpoint = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
#else
    gpio_config_t out{};
    out.pin_bit_mask = 1ULL << kGpio;
    out.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(kGpio, 0);
#endif

    const esp_timer_create_args_t args = {
        .callback = step_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "buzz_step",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_step_timer));
}

void play(padel::sound::Cue cue) {
    std::lock_guard<std::mutex> lock(s_mutex);
    esp_timer_stop(s_step_timer);  // errors only when it was not running
    s_pattern = padel::sound::pattern_for(cue);
    s_index = 0;
    advance_locked();
}

const char* kind() {
#if CONFIG_PADEL_COURT_BUZZER_PASSIVE
    return "passive (LEDC tones)";
#else
    return "active (level only)";
#endif
}

}  // namespace buzzer
