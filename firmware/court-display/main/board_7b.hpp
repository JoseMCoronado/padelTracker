// Board profile for the Waveshare ESP32-S3-Touch-LCD-7B (1024x600 RGB
// panel, GT911 touch, CH32V003 "IO EXTENSION" expander at I2C 0x24).
//
// Pins are transcribed from the vendor wiki pinout table; RGB timings are
// the Waveshare demo set for this panel (30 MHz PCLK), both verified on
// hardware.
#pragma once

#include <cstdint>
#include <optional>

#include "lvgl.h"

namespace board {

constexpr int kHRes = 1024;
constexpr int kVRes = 600;

// Initializes IO expander (backlight + panel power), RGB panel, GT911
// touch, LVGL display + input drivers and the LVGL tick timer. Must be
// called once from the LVGL task's context before any lv_* use.
// Returns false when a step fails (fault is logged).
bool init_display(void);

// Backlight enable via the IO extension (EXIO2 = DISP).
void set_backlight(bool on);

// CH32V003 ADC register 0x06 (10-bit, little-endian). Empty when the
// expander is not ready or the read fails.
std::optional<std::uint16_t> read_battery_raw();

// Battery millivolts via the onboard 3:1 divider (raw * 9900 / 1023).
// Empty when the ADC read fails.
std::optional<std::uint16_t> read_battery_mv();

// Percent 0–100. PWM on register 0x05 is INVERTED (higher duty = dimmer);
// 100% maps to duty ~30, low percent to ~240. Cap duty at 247. 0% turns
// EXIO2 off. See docs/HARDWARE_PINOUT.md.
void set_brightness(std::uint8_t percent);

// Last value passed to set_brightness (defaults to 100 until first call).
std::uint8_t brightness_percent();

// While gated, touches are hidden from LVGL and only latch a wake request, so
// the tap that ends an idle dim can never land on a team panel and score.
void set_touch_gate(bool gated);

// True once since the last call when a gated touch was seen. The gate stays
// on until the finger lifts, so a press held through the wake stays swallowed.
bool consume_touch_wake();

}  // namespace board
