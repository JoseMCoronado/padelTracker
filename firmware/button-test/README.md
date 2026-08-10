# Arcade button bench test (one ESP32-S3 DevKitC-1)

Validates the illuminated arcade buttons — switches and 5V lamps — on a spare
DevKit, and measures the **contact bounce** the product firmware's debounce
constants assume: `stable_press_ms = 150` in
`components/remote_core/include/padel/remote/remote_core.hpp` and the matching
`kWiredButtonPressMs` in `firmware/court-display/main/main.cpp`. Those numbers
were chosen from a first bench run and a court session (ADR-0016); this app is
how they get checked against the real switches.

Edges are timestamped in a GPIO ISR, so a bounce burst resolves to microseconds
instead of to the 5–10 ms poll interval the product firmware runs at.

## Wiring

Per button, two independent circuits. **Board unpowered while wiring.**

- **Switch:** microswitch `COM` to GND, `NO` to the button GPIO. No external
  resistor — the internal pull-up handles it, active low, exactly as
  `docs/HARDWARE_PINOUT.md` specifies for the court unit.
- **Lamp (5V):** DevKit `5V` pin to lamp `+`. Lamp `−` to a low-side switch:
  a logic-level MOSFET (2N7000 / AO3400) gate straight off the lamp GPIO, or a
  BJT (BC337 / 2N2222) base through 1 kΩ. Source/emitter to GND. GPIO high =
  lit. Do **not** drive a 5V lamp straight from a 3.3V GPIO.

Before soldering, hold one lamp directly across `5V` and `GND` to confirm it
lights and to sanity-check the current (a 5V arcade LED is normally ~20 mA with
its resistor built in; the DevKit's USB 5V rail carries five of those).

No transistors on the bench? Wire the lamps permanently across 5V/GND so they
are simply always on and set the lamp GPIOs to `-1`; every switch measurement
below still works.

Defaults (chosen to avoid the DevKitC-1 N16R8 flash/PSRAM pins 26–37, USB
19/20, UART0 43/44, the strapping pins 0/3/45/46 and the onboard RGB LED on 48):

| Button | Switch GPIO | Lamp GPIO |
|---|---|---|
| 1 | 4 | 16 |
| 2 | 5 | 17 |
| 3 | 6 | 18 |
| 4 | 7 | 8 |
| 5 | 15 | 9 |

## Build and flash

```sh
source ~/esp/esp-idf/export.sh
cd firmware/button-test
idf.py set-target esp32s3
idf.py menuconfig            # optional: "Arcade button test"
idf.py -p /dev/cu.usbmodemXXX flash monitor
```

Options under **Arcade button test**: number of buttons (1–5), the five switch
and lamp GPIOs (`-1` disables a lamp), lamp polarity, the settle window
(default 25 ms) and the bounce warning threshold (default 30 ms — the constant
the product firmware assumes).

## What it does

1. **Lamp chase** at boot: each lamp on for 300 ms in turn, so the lamps, their
   drivers and the wiring order are proven before a button is touched. Any
   button already reading pressed at boot is called out as a wiring fault.
2. **Per transition** it logs the press or release with the bounce duration in
   microseconds and the number of edges in the burst:

```text
buttontest: button 2 (GPIO5): press #7, bounce 1840 us over 6 edges
buttontest: button 2 (GPIO5): release after 214 ms, bounce 640 us over 3 edges
```

3. **Glitches** — an edge burst that resolves back to the level it started
   from — are counted and warned about separately. They are harmless here, but
   on a remote each one would burn a retrigger-guard window.
4. **Summary** every 10 s while there is activity, on a BOOT tap, and it ends
   with a verdict against the firmware debounce constant:

```text
buttontest: === summary at 63 s ===
buttontest:  btn gpio  press  relse glitch  maxBounce  avgBounce  edges  minHold  maxHold
buttontest:    1    4     20     20      0     2100 us     890 us      7       98     5210
buttontest: verdict: worst bounce 2100 us, 0 glitches - the 150 ms firmware debounce holds
```

BOOT held for 2 s clears the counters, so you can take a clean run of, say, 50
presses per button.

## Acceptance

- Every button: presses == releases == the number of times you actually pressed
  it, zero glitches.
- Worst bounce comfortably under 150 ms. If the verdict warns, raise
  `stable_press_ms` in `RemoteCoreConfig` and `kWiredButtonPressMs` in
  court-display to match the measurement before trusting the switches — but
  note the ceiling: a press threshold above `undo_hold_ms` (1500 ms) would make
  scoring impossible.
- Minimum hold: deliberate presses should read 190 ms or more in the `minHold`
  column. Anything shorter would be rejected by the 150 ms press threshold.
- Hold one button for 5+ seconds: no release logged in the middle. The pairing
  flow depends on it (`RemoteCoreConfig::pairing_hold_ms = 5000`).
- All five lamps light in the boot chase and then track their own button.

Record the numbers in `STATUS.md` and `docs/HARDWARE_PINOUT.md`.
