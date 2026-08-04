# Waveshare display bring-up runbook (M2)

Prepared before arrival; execute top to bottom when the display is in hand.
Stop point for this phase: static scoreboard + touch verified.

## 1. Confirm the exact model

Read the PCB/SKU label on the back of the board. The purchase listing
suggests the 7-inch **ESP32-S3-Touch-LCD-7B** (1024x600); the sibling
**-7** model is 800x480 and uses a different panel init.

- 7B (1024x600): confirms ADR-0001; wiki page
  <https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7B>
- 7 (800x480): revisit ADR-0001; wiki page
  <https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7>

Note the touch controller (GT911 expected) and whether the board revision
matches the wiki's pinout table.

## 2. Build the untouched vendor demo

1. Download the official demo bundle for the exact model from the wiki
   (ESP-IDF example, not the Arduino one).
2. Build it exactly as documented: note which ESP-IDF version the demo
   declares. If it is not v5.4.x, install that version side by side rather
   than forcing ours (spec version policy).
3. Flash, verify: panel lights up, demo UI renders, touch responds.
4. Record in `docs/TOOLCHAIN.md`: demo bundle version/date, proven ESP-IDF
   version, LVGL version shipped in the demo, and any board-specific
   sdkconfig values (PSRAM mode, flash size — expect octal PSRAM, 8 MB).
5. Finalize ADR-0006 (IDF pin) in `DECISIONS.md`.

## 3. Court-display skeleton (`firmware/court-display`)

- Board profile isolated from app code: one `board/` module owning display
  init (RGB panel timings from the vendor demo), touch init, backlight, and
  LVGL port glue. App code never touches pins.
- Consume `components/` via `EXTRA_COMPONENT_DIRS` like the linktest does
  (`set(COMPONENTS main)` keeps the build lean).
- Render a static scoreboard from a hardcoded `domain::DisplayState`
  (use `domain::project()` output for a mid-match state: 40-30, one set
  each) at 1024x600.
- Touch smoke test: log touch coordinates; verify corners and center.

## 4. Deliverables checklist

- [ ] Model confirmed and recorded (7 vs 7B)
- [ ] Vendor demo built untouched and flashed; versions pinned in TOOLCHAIN.md
- [ ] ADR-0006 finalized
- [ ] Board profile module with display + touch init
- [ ] Static scoreboard rendering from `domain::DisplayState`
- [ ] Touch coordinates verified
- [ ] Results recorded in STATUS.md

Wiring the live `CourtService` into LVGL screens is the next plan, not this
one.
