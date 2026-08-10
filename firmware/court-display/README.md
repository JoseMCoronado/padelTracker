# Padel court display firmware (Waveshare ESP32-S3-Touch-LCD-7B)

The same `CourtService` + journal + LVGL UI that runs in the desktop
simulator, wired to the 7B's RGB panel, GT911 touch, ESP-NOW radio,
LittleFS journal storage, NVS settings, tone buzzer and wired backup buttons.

## Build & flash

```bash
source <IDF_PATH>/export.sh
cd firmware/court-display
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

Managed components (LVGL 8.4, GT911 driver, LittleFS) are fetched by the
IDF component manager on the first build.

## Bring-up day checklist

The only hardware-unverified code is `main/board_7b.cpp` (pins from the
vendor wiki, community-confirmed RGB timings at 16 MHz PCLK) plus the
Kconfig GPIO defaults for buzzer/buttons.

1. Flash the Waveshare vendor demo first; confirm panel + touch work.
2. Diff the demo's timings/pins against `board_7b.cpp`; fix any drift.
3. `idf.py flash monitor` this app; expect the Setup screen.
4. Run the physical acceptance matrix (`docs/RUNBOOKS.md`).

## Task architecture (spec 12.5 / 23.3)

- ESP-NOW receive callback only enqueues into a bounded queue; overflow is
  counted and surfaced on the diagnostics screen.
- The application task owns CourtService, PairingService and the journal;
  it drains the radio and UI-command queues, ticks the conflict window,
  sends ACKs and publishes UiModel snapshots.
- The LVGL task renders snapshots and handles touch. UI callbacks enqueue
  commands; no flash write ever happens on this task.
- The application task is on the task watchdog.

## Configuration (menuconfig -> "Padel Court")

| Option | Default | Notes |
|---|---|---|
| Court id | 1 | |
| Wi-Fi channel | 1 | must match the remotes |
| Buzzer GPIO | 6 | sensor-header pin, the board's only free GPIO |
| Buzzer is passive | y | LEDC tones; `n` drives an active buzzer with levels |
| Wired button A / B GPIO | 13 / 15 | active low; TF card MISO and RS485 TX, both unused |

Not GPIO16 for a button: it is RS485_RXD, driven by the onboard SP3485's
output. See `docs/HARDWARE_PINOUT.md`.

## Sound

`main/buzzer.cpp` is the only code that touches the sounder; what it plays
comes from `components/sound`, which is portable and native-tested, so
court-sim prints the same cues at the same moments. Cues are told apart by
pitch shape rather than length (ADR-0018): a point rises, a remote undo falls,
pairing climbs three notes, a finished match plays a fanfare, and BEEP TEST on
the diagnostics screen sweeps 1-5 kHz to locate the element's resonance.

## Storage

- `storage` partition (8 MB LittleFS) holds `journal.bin`; a fresh match
  archives the old journal to `journal-NNN.bin` (spec 14.7).
- NVS namespace `padel_court` holds the remote allow-list.
- If the LittleFS mount fails the court keeps scoring on a RAM journal and
  latches the storage-fault banner (spec 12.5).
