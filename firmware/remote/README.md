# Padel remote firmware (XIAO ESP32-C3)

Thin wiring layer around the natively tested `components/remote_core`
(spec section 11): ESP-NOW transport, GPIO button, LED feedback, NVS
persistence. All scoring/pairing/retry behavior is covered by the native
test suite; this binary only adapts it to hardware.

## Build & flash

```bash
source <IDF_PATH>/export.sh
cd firmware/remote
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

## Configuration (menuconfig -> "Padel Remote")

| Option | Default | Notes |
|---|---|---|
| Wi-Fi channel | 1 | must match the court |
| Button GPIO | 3 (pad D1) | active low, internal pull-up |
| LED GPIO | 10 (pad D10) | external LED, active high (no user LED on the XIAO) |

Confirm pads against `docs/HARDWARE_PINOUT.md` on bring-up day.

## DevKit stand-in remote (until the XIAOs arrive)

An ESP32-S3 DevKitC-1 with an arcade button on it runs this exact firmware, so
the whole remote path — debounce, retries, pairing, feedback — can be exercised
with a real clicker-shaped press before the XIAOs are on the bench.

`sdkconfig` is tracked and pinned to esp32c3 for the real hardware, so build the
stand-in into a side config and build directory rather than running a bare
`set-target` here:

```bash
cd firmware/remote
idf.py -B build-s3 -D SDKCONFIG=sdkconfig.s3 set-target esp32s3
idf.py -B build-s3 -D SDKCONFIG=sdkconfig.s3 -p /dev/cu.usbmodemXXX flash monitor
```

`sdkconfig.defaults.esp32s3` puts the point button on **GPIO4** and the feedback
LED on **GPIO16** — button 1 of `firmware/button-test`, so the same arcade
button harness moves straight from the bench test to the clicker. The lamp needs
the same low-side transistor as on the bench; wiring is in
[../button-test/README.md](../button-test/README.md). Both `build-s3/` and
`sdkconfig.s3` are gitignored.

Everything else is identical to the XIAO build: hold the arcade button 5 s to
advertise for pairing, then one press = one point with the lamp as the feedback
LED, and a 1.5 s hold takes the last point back again.

## Behavior

- Unpaired: presses give the "pairing required" blink; holding the button
  5 s enters pairing-advertise (broadcasts `PAIR_REQUEST`; the court's
  pairing window + organizer confirmation complete the flow, see
  `docs/PAIRING.md`).
- Paired: one press held at least 150 ms = one point intent with stop-and-wait
  retries (450 ms timeout, 5 attempts). Feedback per the spec 11.3 table.
  The intent goes out when the button is *released*, not when it goes down,
  because a press that keeps going becomes the undo gesture; the press blink
  still fires at the 150 ms mark so it feels immediate. Anything shorter is a
  brush, not a press, and scores nothing (ADR-0016); tune the threshold with
  `CONFIG_PADEL_REMOTE_PRESS_MS` if a court still records phantom points.
- Paired, held 1.5 s: takes back the match's last point whichever team scored
  it (ADR-0014), with a single 300 ms pulse as the cue. It fires once per
  hold, and the court answers a refusal with `RejectedNothingToUndo` (the
  amber "rejected" blink) when there is nothing left to undo.
- The court's MAC is learned from the first ACK / `PAIR_ASSIGN` and
  persisted; intents go unicast afterwards.
- Sequence baselines persist to NVS in chunks of 32 so identities are
  never reused after a reset (spec 11.5).
- Idle 15 min: deep sleeps, waking on the point button (spec 11.4 step 3).
  `RemoteCore::sleep_due()` decides, and refuses while an intent is in
  flight, while advertising, or while the button is down. Unpaired remotes
  sleep too, so one left in a drawer does not drain.
- **The press that wakes it does not score** (ADR-0015) — a wake is a reboot
  and the button is usually released before the firmware is listening, so
  inferring a point would let a knock in a bag add one. Two slow pulses say
  "awake"; press again to score.
- Turn sleep off for bench work with `CONFIG_PADEL_REMOTE_SLEEP_ENABLE=n`,
  or shorten `CONFIG_PADEL_REMOTE_SLEEP_TIMEOUT_S` to exercise it. A
  sleeping remote drops off the serial monitor, which looks like a crash if
  you have forgotten it is enabled.
- Light/modem sleep between points (spec 11.4 step 2, which would extend
  runtime *during* a match rather than standby) is still open.
