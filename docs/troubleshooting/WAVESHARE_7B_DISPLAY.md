# Troubleshooting: Waveshare ESP32-S3-Touch-LCD-7B display

Key learnings from the Aug 2026 debugging session where the display went from
"works all day" to a completely black screen after one power cycle, and the
follow-up flicker hunt. Everything below was verified on our hardware.

All fixes live in `firmware/court-display/main/board_7b.cpp`. Pin map and
register protocol are documented in `docs/HARDWARE_PINOUT.md`.

## TL;DR fix summary

| Symptom | Root cause | Fix |
|---|---|---|
| Black screen on cold boot (worked before unplugging) | Code drove a CH422G that isn't on this board; the real chip (CH32V003) was never commanded, and its volatile state was lost on power-off | Rewrote expander driver for the CH32V003 register protocol at I2C `0x24` |
| Backlight glows but no image ("light towards the center") | EXIO3 is the panel's LCD_RST — it is missing from the wiki pin table, and driving it low holds the panel in reset | Drive EXIO3 high after panel power comes up |
| Whole screen flickers heavily | Writing the expander's PWM register (`0x05`) engages a slow PWM backlight mode | Never write register `0x05`; backlight then runs steady at full brightness |
| Faint whole-panel shimmer | 16 MHz pixel clock = ~17 Hz panel refresh, slow enough for LCD inversion flicker | 30 MHz pixel clock (what Waveshare's demo uses) = ~33 Hz refresh |
| Touch "ABSENT" (`invalid scl frequency`) | `esp_lcd_panel_io_i2c_config_t.scl_speed_hz` left at 0 | Set it to 100 kHz |

## The IO expander is a CH32V003, not a CH422G

This is the single most important learning. Waveshare's wiki for the 7B calls
the chip "IO EXTENSION" and some pages even say CH422G, but the board actually
carries a **CH32V003 microcontroller** running Waveshare's expander firmware.
The two chips have completely incompatible protocols:

- **CH422G** (older Waveshare boards): registers are mapped onto *distinct I2C
  device addresses* (WR_SET `0x24`, WR_IO `0x38`, RD_IO `0x26`).
- **CH32V003** (this board): one device at address `0x24` with normal
  register-pointer reads/writes:
  - `0x02` direction (1 = output) — write `0xFF` at init
  - `0x03` output levels
  - `0x04` input readback (reads actual pin levels — use to verify writes)
  - `0x05` backlight PWM — **do not write, see below**
  - `0x06` ADC (2 bytes, little endian)

Why the bug was invisible at first: the CH422G-style code happened to write
single bytes to device `0x24`, which the CH32V003 ACKs (it's a valid address
for it), so nothing looked wrong on the bus. But no output pin was ever
actually commanded. The display ran on whatever state the expander already
had (left over from the factory demo). **The first real power cycle wiped
that state and the screen went black.** Nothing was broken — the panel had
simply never been powered by *our* firmware.

Lesson: if the display "works" before `init_display()` has demonstrably
configured it, be suspicious — you may be coasting on leftover state.

## EXIO pin map (including the undocumented one)

| Pin | Function | Note |
|---|---|---|
| EXIO1 | TP_RST (touch reset) | Release high after panel power |
| EXIO2 | DISP (backlight enable) | High = on |
| EXIO3 | **LCD_RST (panel reset)** | **Not in the wiki pin table.** Low = backlight-only white screen |
| EXIO4 | TF card CS | Active low; keep high (deselected) |
| EXIO5 | USB/CAN select | Keep low = USB |
| EXIO6 | LCD_VDD_EN (panel VCOM power) | High = on |

The EXIO3 omission cost us an iteration: the rewritten driver set only the
wiki-documented pins and drove everything else low, which held the panel in
reset. Symptom: backlight clearly on (glow, brighter toward center) but no
image. Found via a community driver repo
([holla2040/esp32-s3-touch-lcd-7b](https://github.com/holla2040/esp32-s3-touch-lcd-7b))
that lists IO3 as LCD reset.

Bring-up order that works: panel VDD + backlight enable + SD deselect first
(LCD_RST and TP_RST still low) → 10 ms → release LCD_RST → 100 ms → drive
GT911 INT low, release TP_RST, 50 ms, hand INT back to the driver → 200 ms.

## Never write the backlight PWM register (0x05)

Waveshare's own LCD demo never touches it, and neither should we:

- Unwritten, the backlight runs steady at full brightness.
- Any write engages the chip's PWM mode, whose frequency is low enough to
  flicker visibly. On our hardware this happened with duty 128 **and** 0.
- The mode appears to persist until a full power cycle, so a reflash alone
  won't clear it — unplug completely.
- If dimming is ever genuinely needed: Waveshare's `IO_EXTENSION_Pwm_Output`
  clamps at 97% because values ≥ ~248 turn the screen off entirely. Expect
  flicker as the price of dimming.

## Pixel clock must be 30 MHz

We initially ran 16 MHz ("community-confirmed" for similar panels). With this
panel's timings (1386 × 661 total) that is a **~17 Hz refresh**, and LCDs
develop a faint whole-panel shimmer at such low refresh (polarity-inversion
artifact). It looks like backlight flicker but isn't. Waveshare's demo uses
**30 MHz** (~33 Hz refresh) with the same porch timings
(hsync 162/152/48, vsync 45/13/3) — matching that removed the shimmer.

If the image ever starts drifting sideways or tearing at 30 MHz, that's PSRAM
bandwidth starvation — revisit bounce buffer size (currently 10 lines) and
PSRAM speed settings before lowering the clock.

## GT911 touch quirks

- `esp_lcd_panel_io_i2c_config_t.scl_speed_hz` must be set explicitly
  (100 kHz); leaving it 0 fails with `invalid scl frequency` and touch comes
  up "ABSENT".
- The GT911 latches its I2C address at reset release based on the INT line:
  INT low → `0x5D`, INT high/floating → `0x14`. We drive INT low ourselves
  while releasing TP_RST, and `init_touch()` probes both addresses anyway.

## Debugging techniques that paid off

- **Expander readback:** after writing outputs, read register `0x04` and
  compare with what was written (logged as `IO extension readback ... MATCH`).
  This separates "our command never landed" from "pin is set but the panel
  still doesn't work".
- **"It worked until power-cycled" is a diagnosis**, not a mystery: it means
  the working state was never established by your own code — factory
  firmware leftovers, chip power-on defaults, or warm-reset survivors.
- **Read the vendor's demo source, not just the wiki.** The wiki pin table
  was incomplete (EXIO3) and the chip name was wrong (CH422G vs CH32V003).
  The demo code and Waveshare's ESP component
  ([Waveshare-ESP32-components](https://github.com/waveshareteam/Waveshare-ESP32-components))
  were the ground truth.
- If flashing fails with "port is busy", a leftover `idf.py monitor` process
  is holding the serial port — find it with `lsof /dev/cu.usbmodem*` and kill
  it.
