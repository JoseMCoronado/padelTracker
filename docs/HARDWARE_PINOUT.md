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
| TP_RST | CH422G EXIO1 |
| I2C addresses in use | 0x5D (GT911), 0x24 group (CH422G) |

### CH422G IO expander (fixed)

| Line | Function |
|---|---|
| EXIO1 | Touch reset |
| EXIO2 | DISP (backlight enable) |
| EXIO6 | LCD_VDD_EN (panel VCOM power) |
| EXIO4 | TF card CS (unused) |
| EXIO5 | USB/CAN select (keep low = USB) |

### Project peripherals (chosen by us, `menuconfig` -> "Padel Court")

| Peripheral | Default GPIO | Rationale |
|---|---|---|
| Buzzer (active, high = on) | 6 | free sensor-port pin |
| Arcade button Team A (active low, pull-up) | 16 | RS485 RX pin, RS485 unused |
| Arcade button Team B (active low, pull-up) | 15 | RS485 TX pin, RS485 unused |

Free pins if more are needed: GPIO11/12/13 (TF card, if no card is used),
GPIO19/20 (USB D-/D+, only if native USB is not needed; also CAN).
Almost everything else is consumed by the panel.

## Remote — Seeed XIAO ESP32-C3

Chosen by us, `menuconfig` -> "Padel Remote":

| Peripheral | Default GPIO | XIAO pad |
|---|---|---|
| Point button (active low, internal pull-up) | 3 | D1 |
| Feedback LED (active high, external) | 10 | D10 |

The XIAO ESP32-C3 has **no user LED** on board — wire an LED + resistor to
D10 (or change the Kconfig). The button wires between D1 and GND. Haptic
motor driver pin will be chosen at M6 when sleep/haptics are enabled.

## Radio

ESP-NOW on Wi-Fi channel 1 (both `menuconfig`s must match). MACs are
learned at runtime (court learns remotes from packets; remotes learn the
court from its first ACK / PAIR_ASSIGN) — no MACs are hardcoded.
