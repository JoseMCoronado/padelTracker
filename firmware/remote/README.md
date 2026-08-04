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

## Behavior

- Unpaired: presses give the "pairing required" blink; holding the button
  5 s enters pairing-advertise (broadcasts `PAIR_REQUEST`; the court's
  pairing window + organizer confirmation complete the flow, see
  `docs/PAIRING.md`).
- Paired: one debounced press = one point intent with stop-and-wait
  retries (450 ms timeout, 5 attempts). Feedback per the spec 11.3 table.
- The court's MAC is learned from the first ACK / `PAIR_ASSIGN` and
  persisted; intents go unicast afterwards.
- Sequence baselines persist to NVS in chunks of 32 so identities are
  never reused after a reset (spec 11.5).
- Sleep is deliberately not implemented yet (spec 11.4 ordering); the
  hook exists in `main.cpp`.
