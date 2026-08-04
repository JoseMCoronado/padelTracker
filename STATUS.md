# Current milestone

M0/M1 — Repository, scoring engine, simulator: COMPLETE
M2/M3 prep — Application layer, persistence, radio simulation: COMPLETE (native)
M3–M7 software — UI, remote core, pairing, both firmware apps, observability:
COMPLETE (native + firmware builds; hardware verification pending)
Next: hardware arrival — flash-and-verify only

# Working

- Native domain scoring engine (advantage, golden point, sets, tiebreaks,
  match tiebreak, club mini-set preset) — `components/domain`
- Undo via compensating events + journal replay, across all boundaries
- Protocol: POINT_INTENT/ACK + PAIR_REQUEST/PAIR_ASSIGN with CRC16, golden
  vectors, wrap-safe deduplicator — `components/protocol`
- Application layer — `components/application`: CourtService single entry
  point, conflict guard (ADR-0009), PairingService with persisted
  allow-list (ADR-0011)
- Persistence — `components/persistence`: CRC-framed journal, recovery,
  power-loss fault matrix green (ADR-0005)
- Remote core — `components/remote_core`: portable state machine (debounce
  30/30/700 ms, stop-and-wait retries, feedback table, NVS sequence
  baselines, pairing states), fully tested natively
- Full LVGL v8.4.0 UI — `components/ui` (ADR-0010): setup, live match,
  match complete, undo preview, protected reset, pairing, diagnostics,
  recovery; view models projected from CourtService; headless render tests
  at 1024x600 with stress content (spec 18.6 subset)
- Desktop court — `simulator/court-sim`: SDL window, keyboard remotes
  running real RemoteCore, loss injection, journal-backed power cycle,
  pairing flow, `--tour` screenshot mode
- Native E2E: RemoteCore vs CourtService over the lossy channel with
  mid-run reboots — exactly-once proven with real remote logic (M4
  acceptance rehearsal)
- Structured logging (spec 16): stable event names, bounded ring buffer,
  surfaced on the diagnostics screen + serial/stdout sink; native-tested
- `firmware/remote` (XIAO ESP32-C3): remote_core + ESP-NOW + button + LED +
  NVS — builds clean for esp32c3
- `firmware/court-display` (Waveshare 7B): board profile (unverified pins/
  timings isolated in `board_7b.cpp`), radio-callback -> queue -> app task ->
  LVGL task split (ADR-0012), LittleFS journal + NVS settings, buzzer +
  arcade buttons, watchdog — builds clean for esp32s3
- 155 native tests passing (domain, protocol, application, persistence,
  integration, remote, ui, common)
- ESP-IDF v5.4.4; all three firmware projects build

# In progress

- (nothing — remaining work is hardware-gated)

# Blocked (hardware not yet arrived)

- ESP-NOW 500-press soak on the DevKits (`firmware/espnow-linktest/README.md`)
- Waveshare 7B bring-up: vendor demo, then verify `board_7b.cpp` pins +
  timings (`docs/WAVESHARE_BRINGUP.md`, `docs/HARDWARE_PINOUT.md`)
- Remote physical acceptance: range, latency, battery; haptics + sleep (M6)
- Encrypted ESP-NOW peers (keys scheme in `docs/PAIRING.md`)

# Next three tasks (hardware day)

1. DevKits: run the linktest soak, record applied/duplicate/latency here
2. 7B: vendor demo -> board profile verification -> flash court-display ->
   Setup screen -> touch + buzzer + arcade buttons
3. XIAO: flash remote, pair via the on-screen flow, run the M4 acceptance
   matrix physically (loss, reboot, conflict, duplicate press)

# Last verified commands

- `tools/run_native_tests.sh` — 155/155 tests pass (2026-08-04)
- `./build/native/simulator/court-sim/court-sim --tour` — all 8 screens render
- `idf.py build` in `firmware/court-display` (esp32s3) — clean (2026-08-04)
- `idf.py build` in `firmware/remote` (esp32c3) — clean (2026-08-04)
