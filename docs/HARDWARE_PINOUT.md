# Hardware pinout

Pin assignments for the P0 prototype. Sources: Waveshare ESP32-S3-Touch-LCD-7B
wiki pinout tables and the Seeed XIAO ESP32-C3 pin map.

> **Every value here is unverified until hardware bring-up.** The firmware
> keeps all of it isolated: the 7B panel pins/timings live only in
> `firmware/court-display/main/board_7b.cpp`, and the freely chosen pins
> (buzzer, arcade buttons, remote button/LED) are `menuconfig` options.
> Bring-up day: run the vendor demo first, diff, fix, re-flash.

## Court unit — Waveshare ESP32-S3-Touch-LCD-7B

### RGB LCD (fixed by the board, `board_7b.cpp`)

| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| HSYNC | 46 | | B3..B7 | 14, 38, 18, 17, 10 |
| VSYNC | 3 | | G2..G7 | 39, 0, 45, 48, 47, 21 |
| DE | 5 | | R3..R7 | 1, 2, 42, 41, 40 |
| PCLK | 7 | | | |

Timings (community-confirmed for this 1024x600 panel, verify against the
vendor demo): PCLK 16 MHz active-low; HSYNC pulse/back/front 162/152/48;
VSYNC pulse/back/front 45/13/3. Frame buffer in PSRAM, bounce buffer
1024x10 px.

### Touch — GT911 (fixed)

| Signal | Pin |
|---|---|
| I2C SDA / SCL | GPIO8 / GPIO9 (shared bus with the IO expander) |
| TP_IRQ | GPIO4 |
| TP_RST | IO extension EXIO1 |
| I2C addresses in use | 0x5D or 0x14 (GT911), 0x24 (IO extension) |

### IO extension — CH32V003 (fixed)

Despite older Waveshare boards using a CH422G, the 7B's "IO EXTENSION" is a
CH32V003 microcontroller at I2C address 0x24 with a normal register protocol:
0x02 direction (1 = output), 0x03 output levels, 0x04 input readback,
0x05 backlight PWM (**inverted**: higher duty = dimmer; usable ~30 full …
240 dim, never ≥ 248), 0x06 ADC (battery voltage via onboard 3:1 divider).

| Line | Function |
|---|---|
| EXIO1 | Touch reset |
| EXIO2 | DISP (backlight enable) |
| EXIO3 | LCD_RST (not in the wiki table; panel is backlight-only white while low) |
| EXIO4 | TF card CS (active low, unused — keep high) |
| EXIO5 | USB/CAN select (keep low = USB) |
| EXIO6 | LCD_VDD_EN (panel VCOM power) |

Quirks learned on hardware: the MCU firmware wants a short pause (~2 ms)
between I2C transactions. Backlight PWM polarity is inverted (AP3032 FB):
writing duty 247 for "100%" blanks the panel — that duty is minimum light.
The ORGANIZER slider maps 10–100% onto duty ~219–30 and persists the percent
in NVS. Unwritten `0x05` is also full brightness (Waveshare demo behaviour).

### Court Li-ion (PH2.0)

The 7B has an onboard CS8501 charge/boost and a PH2.0 battery header for a
single-cell 3.7 V Li-ion. P0 cell: **CITYORK 3.7 V 2000 mAh 103450**. Verify
connector polarity before first connect — do not assume all PH/JST-style
connectors share the same polarity.

Battery voltage is read from expander register `0x06` (10-bit LE). Millivolts:

`mv = raw * 9900 / 1023` (3.3 V reference × 3:1 divider).

SoC is a piecewise OCV curve in `padel/common/battery.hpp`; estimated runtime
uses rated capacity × SoC / an assumed ~475 mA cell draw (from the board’s
5 V / 350 mA figure through boost). Readings below ~2.5 V are treated as
no cell / unknown.

### Project peripherals (chosen by us, `menuconfig` -> "Padel Court")

| Peripheral | Default GPIO | Rationale |
|---|---|---|
| Buzzer (passive, LEDC square wave) | 6 | free sensor-header pin |
| Arcade button Team A (active low, pull-up) | 13 | TF card MISO; no card is used |
| Arcade button Team B (active low, pull-up) | 15 | RS485 TX pin, RS485 unused |

> **Do not put a button on GPIO16.** It is `RS485_RXD`, wired to the *output*
> of the board's SP3485 transceiver (Waveshare wiki, "RS485 port"). The pin is
> actively driven, so a switch pulling it to ground fights a push-pull output:
> the press may never read low, and current flows for as long as it is held.
> GPIO15 is the opposite case — `RS485_TXD` feeds the transceiver's
> high-impedance input, so repurposing it costs nothing while RS485 is unused.

Free pins if more are needed: GPIO11/12 (the rest of the TF card group, since
the journal lives on internal-flash LittleFS), GPIO19/20 (USB D-/D+, only if
native USB is not needed; also CAN). Almost everything else is consumed by the
panel.

### Buzzer

GPIO6 is the only fully free GPIO on the board, and it is brought out on the
**sensor header** — a 3-pin HY2.0 connector carrying 3V3 / GPIO6 / GND, with a
HY2.0-to-Dupont cable in the box, so nothing needs soldering. Check the pin
order against the silkscreen before connecting.

The firmware drives a **passive** sounder: it generates the frequency itself
with LEDC so each cue gets its own pitch (ADR-0018). An active buzzer has its
own oscillator and would ignore the pitch, so it needs
`PADEL_COURT_BUZZER_PASSIVE=n`, which falls back to level-only drive where the
cues differ by rhythm alone.

- **Direct drive** works only for a low-current 3.3 V piezo, roughly 3-10 mA:
  `+` to GPIO6, `-` to the header's GND. Check the datasheet current first; the
  S3 pin is rated 40 mA absolute maximum and should not run near it.
- **Transistor drive** is what a hall needs. A loud electromagnetic sounder
  pulls 25-40 mA at 5 V, so use the same low-side switch as the arcade lamps:
  5 V (from the 5 V output header) to buzzer `+`, buzzer `-` to a
  2N7000/AO3400 drain or a BC337 collector through 1 kΩ, source/emitter to GND,
  gate/base to GPIO6. Add a 1N4148 across a coil-type sounder, cathode to +5 V.
- **Fit a 10 kΩ pulldown** from GPIO6 to GND either way (gate-to-GND for the
  transistor version). The pin floats from power-on until `buzzer::init()`
  runs, so without it every reset and reflash squawks.

Piezo elements are loud only near their mechanical resonance, typically around
4 kHz, and fall off steeply either side, which is why the cue tones all sit in
the 1-5 kHz band. The diagnostics BEEP TEST plays a stepped sweep across that
band for exactly this reason: whichever step rings loudest is the resonance,
and the cue pitches in `components/sound` should sit near it.

## Arcade buttons (5x 30 mm illuminated)

Switch and lamp are separate circuits inside the button.

- **Switch**: microswitch `COM` to GND, `NO` to the input GPIO. Active low on
  the internal pull-up; no external resistor anywhere in this project.
- **Lamp (5V)**: 5V to lamp `+`; lamp `-` to a low-side switch — logic-level
  MOSFET (2N7000/AO3400) gate straight off a GPIO, or BJT (BC337/2N2222) base
  through 1 kΩ — source/emitter to GND. GPIO high = lit. A 3.3V GPIO cannot
  drive a 5V lamp directly. Roughly 20 mA per lamp, resistor built in.

The court unit firmware drives no lamps today: it has two switch inputs
(GPIO13/15) and the buzzer, nothing else. On a remote the lamp doubles as the
feedback LED via `PADEL_REMOTE_LED_GPIO`.

### Bench harness on an ESP32-S3-DevKitC-1

`firmware/button-test` exercises all five buttons on a spare DevKit and
measures contact bounce. Its defaults avoid the DevKitC-1 N16R8 flash/octal
PSRAM pins (26-37), USB (19/20), UART0 (43/44), the strapping pins (0/3/45/46)
and the onboard RGB LED (48):

| Button | Switch GPIO | Lamp GPIO |
|---|---|---|
| 1 | 4 | 16 |
| 2 | 5 | 17 |
| 3 | 6 | 18 |
| 4 | 7 | 8 |
| 5 | 15 | 9 |

Button 1's pins are also what `firmware/remote` uses when built for esp32s3
(`sdkconfig.defaults.esp32s3`), so one harness serves both the bench test and
the DevKit stand-in clicker.

### Measured bounce (2026-08-05, first run — provisional)

| Button | GPIO | Presses | Avg bounce | Max bounce | Max edges | Glitch bursts |
|---|---|---|---|---|---|---|
| 1 | 4 | 96 | 21 ms | 389 ms | 5945 | 142 |
| 2 | 5 | 25 | 1.6 ms | 25 ms | 77 | 4 |
| 3 | 6 | 22 | 2.8 ms | 40 ms | 65 | 6 |
| 4 | 7 | 21 | 4.0 ms | 74 ms | 90 | 10 |
| 5 | 15 | 25 | 4.8 ms | 29 ms | 56 | 9 |

Presses equalled releases on every button, so nothing was lost or doubled at
the 25 ms settle window. The averages include glitch bursts, which inflates
them.

Why provisional: the harness was alligator clips and breadboard jumpers rather
than crimped terminals, and button 1 was handled throughout the wiring
session. Its 5945-edge, 389 ms burst is connection chatter, not switch bounce
— disregard that row. Buttons 2-5 are the usable sample.

What it means:

- Typical bounce is 1.6-4.8 ms, which the assumed 30 ms covers easily.
- Worst-case bursts reach 25-74 ms, which it does not. A burst longer than the
  debounce window can present as press-release-press, i.e. one physical press
  scoring twice. The bench tool records burst duration and edge count, not the
  level pattern inside the burst, so this is a risk rather than a proof.
- The remote is already protected: `RemoteCoreConfig::retrigger_guard_ms = 700`
  suppresses a second accepted press inside 700 ms. The court unit's
  `WiredButton` has no equivalent guard, and that gap is confirmed in code, not
  just suspected: given two presses of the same team inside the conflict
  window, `CourtService::award_point_local` commits the parked first press and
  parks the second, so both score. A bounce burst split into two presses is a
  silent double point.

Debounce constants are deliberately unchanged until a re-run on a solid
harness says what the real worst case is.

Before these switches are trusted: re-run with crimped or soldered
connections, and consider 100 nF across each switch (`COM`-`NO`) plus a 10 kΩ
external pull-up for any cable run longer than a few tens of centimetres — the
internal pull-up is only ~45 kΩ, which is weak for a metre of unshielded wire
in a sports hall.

## Remote — Seeed XIAO ESP32-C3

Chosen by us, `menuconfig` -> "Padel Remote":

| Peripheral | Default GPIO | XIAO pad |
|---|---|---|
| Point button (active low, internal pull-up) | 3 | D1 |
| Feedback LED (active high, external) | 10 | D10 |

The XIAO ESP32-C3 has **no user LED** on board — wire an LED + resistor to
D10 (or change the Kconfig). The button wires between D1 and GND. Haptic
motor driver pin will be chosen at M6 when haptics are enabled.

### D1 wiring is the weak point (2026-08-05, twice in one session)

The remote's button connection failed twice on bring-up day: first as an open
circuit before the headers were soldered, then as an **intermittent** that
appeared mid-session after a few hundred presses, where wiggling the wiring
restored contact. Both times the symptom looked like a firmware fault.

Diagnose it from the heartbeat rather than guessing. `gpio=` is the true pin
level and `raw=` is what `remote_core` was told:

| `gpio=` at rest | `gpio=` when pressed | Meaning |
|---|---|---|
| 1 | 0 | Healthy |
| 1 | 1 (never drops) | Open circuit: joint, lead or switch |
| 0 | 0 (always) | Short to GND, or a stuck switch |

If wiggling the wire makes it fire, it is a cold joint or a loose spade
terminal, not the board — a pin that reaches 0 at all proves the pad, the
joint and the firmware path are fine. Reflow D1 and GND, and add strain
relief so wire flex does not load the joint. The enclosure design should
anchor the cable rather than let it hang off the solder pads.

### Deep-sleep wake pin (verified, spec 11.4)

**GPIO3 / pad D1 is wake-capable.** Only GPIO0-5 can wake an ESP32-C3 from
deep sleep, confirmed against the IDF SoC capability header rather than
assumed:

```
SOC_GPIO_DEEP_SLEEP_WAKE_VALID_GPIO_MASK  (BIT0 | BIT1 | BIT2 | BIT3 | BIT4 | BIT5)
SOC_GPIO_DEEP_SLEEP_WAKE_SUPPORTED_PIN_CNT (6)
```

The point button therefore doubles as the wake source, low-level triggered
(ADR-0015). No external pull-up is needed: `ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS`
is on by default and `esp_deep_sleep_start` applies the pull-up itself. If a
long cable run ever causes spurious wakes, add a 10 kΩ external pull-up
**and** disable that option, since combining internal and external resistors
is what the IDF warns against.

Moving the button off GPIO0-5 produces a remote that sleeps and never wakes.
A `static_assert` in `firmware/remote/main/main.cpp` fails the build rather
than letting that ship.

**Confirmed on hardware 2026-08-05**, unit 1 with a 60 s bench timeout: the
remote slept on schedule and the arcade switch on D1 woke it, with the chip
clock restarting from zero.

One consequence to expect when monitoring. The XIAO has no USB-UART bridge —
the console is the C3's own USB Serial/JTAG — so **the serial port disappears
from the host while the remote sleeps** and re-enumerates on wake, possibly
under a different name. A vanished port is normal sleep behaviour, not a
crash. Two things follow: `esptool` cannot reach a sleeping remote, so wake
it with a press before flashing; and early-boot log lines are lost every
time, because the host has not finished enumerating the device yet. The
sleep message survives only because `enter_deep_sleep()` drains the console
before powering down.

## Radio

ESP-NOW on Wi-Fi channel 1 (both `menuconfig`s must match). MACs are
learned at runtime (court learns remotes from packets; remotes learn the
court from its first ACK / PAIR_ASSIGN) — no MACs are hardcoded.
