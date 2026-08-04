#pragma once

#include <cstdint>

#include "lvgl.h"

// Centralized design tokens (spec section 14.2). Every screen pulls sizes,
// colors, and fonts from here; no screen hardcodes styling.
namespace padel::ui::tokens {

// --- Layout ------------------------------------------------------------
inline constexpr lv_coord_t kScreenWidth = 1024;
inline constexpr lv_coord_t kScreenHeight = 600;
inline constexpr lv_coord_t kSpaceXs = 4;
inline constexpr lv_coord_t kSpaceS = 8;
inline constexpr lv_coord_t kSpaceM = 16;
inline constexpr lv_coord_t kSpaceL = 24;
inline constexpr lv_coord_t kSpaceXl = 40;
inline constexpr lv_coord_t kRadius = 12;
// Spec: minimum touch target 48 px, larger for organizer actions.
inline constexpr lv_coord_t kTouchTarget = 48;
inline constexpr lv_coord_t kOrganizerTarget = 64;

// --- Animation ----------------------------------------------------------
inline constexpr std::uint32_t kPointPulseMs = 200;  // 150-250 ms per spec 14.3

// --- Colors (dark, high-contrast; labels/shapes carry meaning, not color
// alone per spec 14.1) ----------------------------------------------------
inline lv_color_t bg() { return lv_color_hex(0x101418); }
inline lv_color_t surface() { return lv_color_hex(0x1C232B); }
inline lv_color_t surface_raised() { return lv_color_hex(0x27313C); }
inline lv_color_t text() { return lv_color_hex(0xF2F5F7); }
inline lv_color_t text_muted() { return lv_color_hex(0x93A1AD); }
inline lv_color_t team_a() { return lv_color_hex(0x2F80ED); }   // blue
inline lv_color_t team_b() { return lv_color_hex(0xF2994A); }   // orange
inline lv_color_t success() { return lv_color_hex(0x27AE60); }
inline lv_color_t warning() { return lv_color_hex(0xE2B93B); }
inline lv_color_t error() { return lv_color_hex(0xEB5757); }

// --- Fonts ----------------------------------------------------------------
inline const lv_font_t* font_small() { return &lv_font_montserrat_14; }
inline const lv_font_t* font_body() { return &lv_font_montserrat_16; }
inline const lv_font_t* font_heading() { return &lv_font_montserrat_20; }
inline const lv_font_t* font_large() { return &lv_font_montserrat_28; }
inline const lv_font_t* font_score() { return &lv_font_montserrat_48; }
// The point score must be readable across a court: 48 pt scaled 3x via
// transform zoom (~144 px). A dedicated big font is a later polish item.
inline constexpr std::uint16_t kScoreZoom = 256 * 3;

}  // namespace padel::ui::tokens
