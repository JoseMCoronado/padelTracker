# Getting started: from boxed hardware to a working court display

The simplified, in-order version of what it actually took to kick-start this
project on real hardware (macOS, Aug 2026). Each step worked — the detours and
bugs we hit along the way are written up in
[troubleshooting/WAVESHARE_7B_DISPLAY.md](troubleshooting/WAVESHARE_7B_DISPLAY.md).

Hardware used: 3x ESP32-S3-DevKitC-1, 1x Waveshare ESP32-S3-Touch-LCD-7B
(1024x600 touch screen), arcade buttons + buzzer. The Seeed XIAO remotes were
missing, so a spare DevKit stands in as the remote.

## Golden rules (learned the hard way)

- Every **new terminal window** needs `source ~/esp/esp-idf/export.sh` before
any `idf.py` command works.
- Exit the serial monitor with **Ctrl + ]** (not Ctrl+C).
- Board won't flash / "failed to connect": hold **BOOT**, tap **RESET**,
release BOOT, retry.
- "Port is busy" when flashing: an old monitor still owns the port — find it
with `lsof /dev/cu.usbmodem*` and kill that process.
- Find a board's port name: `ls /dev/cu.*` before and after plugging in; the
new entry is your board.
- Use a **data** USB-C cable (one that can sync a phone). Charge-only cables
are the #1 cause of "board doesn't show up".



## Step 0 — Prove the computer is ready (no hardware, ~10 min)

```bash
cd ~/Documents/git/padelTracker
tools/run_native_tests.sh          # must end with all tests passed
source ~/esp/esp-idf/export.sh     # ESP-IDF installed at ~/esp/esp-idf
cd firmware/espnow-linktest
idf.py set-target esp32s3
idf.py build                       # first build takes minutes; "Project build complete"
```

If tests pass and the build completes, any later problem is hardware or
config, not code.

## Step 1 — Radio proof with two DevKits (~1 hour)

Goal: prove "N button presses = exactly N points, even with bad reception"
before risking the expensive screen.

1. Plug DevKit #1 into the Mac via its **UART** USB-C socket; note its port
  (`PORT1`).
2. In `firmware/espnow-linktest`: `idf.py menuconfig` → ESP-NOW linktest
  configuration → Role → **Receiver (plays court)** → save, quit. Then
   `idf.py -p PORT1 flash monitor`. Leave it listening.
3. Plug DevKit #2 (second terminal, `source` the export script again, port =
  `PORT2`). Same menuconfig → Role → **Sender (plays remote)**. Then
   `idf.py -p PORT2 flash monitor`.
4. The sender fires 500 simulated presses; the receiver deliberately drops 20%
  of ACKs to force retries. Success = `[final] presses=500 accepted=500 ...  failed=0`. Record the latency numbers in `STATUS.md`.



## Step 2 — The Waveshare screen (~1-2 hours)

1. Unplug the DevKits first (avoids port confusion).
2. Check the sticker on the back: must say **ESP32-S3-Touch-LCD-7B** (the
  plain `-7` is a different panel).
3. Plug the screen into the Mac. Either USB-C port powers it; flashing works
  through the **USB** port (shows up as `/dev/cu.usbmodem...` = `SCREENPORT`).
4. Erase once (the factory demo's flash layout confuses our storage):

```bash
cd ~/Documents/git/padelTracker/firmware/court-display
idf.py -p SCREENPORT erase-flash
```

1. Flash ours:

```bash
idf.py -p SCREENPORT flash monitor
```

1. Success looks like this in the log, with the setup screen on the panel:

```text
board7b: IO extension readback: wrote 0x5E, pins read 0x5E (MATCH)
board7b: GT911 up at 0x5D
board7b: display up: 1024x600, touch ok
```

1. Touch test: tap all four corners and the center — the button under your
  finger should react, not an offset one.

If instead the screen is black, glows without an image, flickers, or touch is
ABSENT: it's all been solved before, see
[troubleshooting/WAVESHARE_7B_DISPLAY.md](troubleshooting/WAVESHARE_7B_DISPLAY.md).

## Step 3 — Play a real match on it (~1 hour)

1. Using only the touchscreen: set up a match, award points (tap a team's
  score panel), undo, pause, finish a set.
2. **Power-cut test:** mid-match, yank the cable, plug back in. The Recovery
  screen must offer to resume with the exact score intact.
3. **Standalone test:** unplug everything, power from a power bank only (USB
  port). It must boot to the setup screen by itself. No shutdown procedure is
   needed before unplugging — ever.
4. Walk the club flow (player picker, mix screen, standings) with real
  fingers — it's the newest UI code.



## Step 4 — Bench-test the arcade buttons on a DevKit (~30 min)

Do this before the buttons go anywhere near the screen: a spare DevKit tells
you whether the switches and lamps are good, and measures the contact bounce
the firmware's 30 ms debounce assumes.

Wiring per button, board **unpowered** (full details in
[../firmware/button-test/README.md](../firmware/button-test/README.md)):
switch `COM` to GND and `NO` to the button GPIO — no resistor; lamp `+` to the
DevKit `5V` pin and lamp `-` through a low-side transistor to GND, gate/base
from the lamp GPIO. Defaults are switches on GPIO 4/5/6/7/15 with lamps on
16/17/18/8/9.

```bash
cd ~/Documents/git/padelTracker/firmware/button-test
idf.py set-target esp32s3
idf.py -p PORT flash monitor
```

All five lamps chase at boot, then each press logs its bounce in microseconds.
Give every button ~20 presses and one 5-second hold, then tap BOOT for the
summary. The last line is the verdict: if it warns that bounce reached the
150 ms press threshold, raise `stable_press_ms` in `components/remote_core` and
`kWiredButtonPressMs` (`firmware/court-display/main/main.cpp`) before going
further.
Record the numbers in [HARDWARE_PINOUT.md](HARDWARE_PINOUT.md).

## Step 5 — Wire the extras to the court unit (separate session)

With the board **unpowered**: buzzer between **GPIO6** and GND (mind
polarity), arcade button Team A between **GPIO13** and GND, Team B between
**GPIO15** and GND. No resistors — internal pull-ups handle it. Not GPIO16:
that pin is driven by the board's RS485 transceiver output, see
[HARDWARE_PINOUT.md](HARDWARE_PINOUT.md). Power up; the
Diagnostics screen has a test beep and shows each wired button's live state
and press count, so you can prove the wiring without scoring a point. Then
press them for real — each one scores for its team.

## Step 6 — Spare DevKit as the missing remote

Until the Seeed XIAO remotes arrive, the real remote firmware runs on a spare
DevKit. `sdkconfig` in that project is pinned to esp32c3 for the real XIAOs, so
build the stand-in into a side config that leaves it alone:

```bash
cd ~/Documents/git/padelTracker/firmware/remote
idf.py -B build-s3 -D SDKCONFIG=sdkconfig.s3 set-target esp32s3
idf.py -B build-s3 -D SDKCONFIG=sdkconfig.s3 -p PORT3 flash monitor
```

That build puts the point button on **GPIO4** and the feedback LED on
**GPIO16** — button 1 of the Step 4 harness, so an arcade button plus its lamp
becomes the clicker with no rewiring. Prefer no wiring at all? Run
`idf.py -B build-s3 -D SDKCONFIG=sdkconfig.s3 menuconfig` and set the point
button GPIO to 0, which is the DevKit's BOOT button.

Then pair it from the court display's organizer menu (see
[PAIRING.md](PAIRING.md)): hold the button 5 s to advertise. One press = one
point, and the lamp gives the feedback pattern. Worth checking by hand: a
double-tap inside 700 ms scores once, and two remotes pressed together raise
the conflict prompt.

Holding the button 1.5 s takes the last point back again (ADR-0014), whichever
team scored it, which the court confirms with a long 500 ms beep. Two things to
try: hold after the *other* team scored — that point should come off too — and
hold for ten seconds straight, which must still only remove one point.

While you are here, check the press threshold from the same session: a quick
brush across the button should score nothing, and a normal press should feel
instant. That is the 150 ms of contact a press now needs (ADR-0016).