#pragma once

#include "lvgl.h"

// Digits-only Montserrat for court-readable point scores (currently 300 px).
// Glyphs: 0-9, A, D (covers "0"/"15"/"30"/"40"/"AD" and tiebreak digits).
// Regenerate with fonts/generate_score_font.sh.
LV_FONT_DECLARE(lv_font_montserrat_score);
