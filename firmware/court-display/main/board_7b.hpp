// Board profile for the Waveshare ESP32-S3-Touch-LCD-7B (1024x600 RGB
// panel, GT911 touch, CH32V003 "IO EXTENSION" expander at I2C 0x24).
//
// Pins are transcribed from the vendor wiki pinout table; RGB timings are
// the community-confirmed set for this panel (16 MHz PCLK), both verified
// on hardware.
#pragma once

#include "lvgl.h"

namespace board {

constexpr int kHRes = 1024;
constexpr int kVRes = 600;

// Initializes IO expander (backlight + panel power), RGB panel, GT911
// touch, LVGL display + input drivers and the LVGL tick timer. Must be
// called once from the LVGL task's context before any lv_* use.
// Returns false when a step fails (fault is logged).
bool init_display(void);

// Backlight control via the IO extension (EXIO2 = DISP).
void set_backlight(bool on);

}  // namespace board
