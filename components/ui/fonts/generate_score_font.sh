#!/usr/bin/env bash
# Regenerate the court score font (digits + AD only). Requires Node/npx.
set -euo pipefail
cd "$(dirname "$0")"

SIZE="${SCORE_FONT_SIZE:-300}"

npx --yes lv_font_conv \
  --no-compress \
  --no-prefilter \
  --bpp 4 \
  --size "$SIZE" \
  --font Montserrat-Medium.ttf \
  --symbols '0123456789AD' \
  --format lvgl \
  --lv-font-name lv_font_montserrat_score \
  --lv-include lvgl.h \
  --force-fast-kern-format \
  -o lv_font_montserrat_score.c

echo "Wrote lv_font_montserrat_score.c (size=${SIZE}px)"
