# Current milestone

M0/M1 — Repository, scoring engine, simulator: COMPLETE
M2/M3 prep — Application layer, persistence, radio simulation: COMPLETE (native)
M3–M7 software — UI, remote core, pairing, both firmware apps, observability:
COMPLETE (native + firmware builds; hardware verification pending)
Next: hardware arrival — flash-and-verify only

# Working

- Native domain scoring engine (advantage, golden point, sets, tiebreaks,
  match tiebreak, club mini-set preset) — `components/domain`
- Undo via compensating events + journal replay, across all boundaries;
  optionally scoped to one team so a remote can only take back its own point
- Protocol: POINT_INTENT/ACK + PAIR_REQUEST/PAIR_ASSIGN with CRC16, golden
  vectors, wrap-safe deduplicator — `components/protocol`
- Application layer — `components/application`: CourtService single entry
  point, conflict guard (ADR-0009), PairingService with persisted
  allow-list (ADR-0011)
- Persistence — `components/persistence`: CRC-framed journal, recovery,
  power-loss fault matrix green (ADR-0005)
- Remote core — `components/remote_core`: portable state machine (debounce
  30/30/700 ms, stop-and-wait retries, feedback table, NVS sequence
  baselines, pairing states, inactivity-sleep decision), fully tested natively
- Remote inactivity deep sleep (ADR-0015, spec 11.4 step 3): idle 15 min and
  the remote deep sleeps, waking on the point button. `sleep_due()` refuses
  while an intent is in flight, while advertising, or while the button is
  down; unpaired remotes sleep too. The waking press deliberately does not
  score. Toggle with `CONFIG_PADEL_REMOTE_SLEEP_ENABLE`.
  **Verified on hardware 2026-08-05** (unit 1, 60 s bench timeout): slept on
  schedule, woke on the GPIO3 press with the chip clock restarting from zero,
  that press left `presses=0 intents=0`, and the next press scored and was
  ACKed by the court in 490 ms — well inside the ~3.5 s a retry exhaustion
  would have taken, so the point genuinely reached the display
- Remote hold-to-undo (ADR-0014): a 3 s hold takes back that team's own last
  point via `Action::UndoLastPoint` on the POINT_INTENT frame. Award intents
  now go out on button release rather than press-down so a hold can still
  become an undo; the court beeps 500 ms so it is never mistaken for a score.
  A deliberate departure from spec 11.2/14.6 — see the ADR
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
- Club round (ADR-0013): `domain::ClubRound` (first-to-3 x2, winners-split
  mix, wins + differential standings, Top2/Bottom2, automatic announced
  coin flip on ties) validated against the rotation sheets;
  `PlayerRoster` (seeded Jose/Zoe/William/Szewei/Ruxandra +
  Lewis/Luigi/Raymond/Paulina/Vineet/Louis/Adrien, add players
  on device, session guests) + CSV results log; touch player picker
  (search, tiles, NEW PLAYER, ADD GUEST), mix + standings screens; wired
  into both court-sim and court-display, forbidden-pair guard between
  rounds
- `firmware/remote` (XIAO ESP32-C3): remote_core + ESP-NOW + button + LED +
  NVS — builds clean for esp32c3. Fixed on hardware 2026-08-05: the main
  loop's `vTaskDelay(pdMS_TO_TICKS(5))` rounded to zero ticks at the default
  100 Hz tick, so the task never blocked and the idle task starved until the
  watchdog fired every 5 s. Delay is now clamped to at least one tick; this
  affected the esp32c3 build identically
- `firmware/court-display` (Waveshare 7B): board profile (unverified pins/
  timings isolated in `board_7b.cpp`), radio-callback -> queue -> app task ->
  LVGL task split (ADR-0012), LittleFS journal + NVS settings, buzzer +
  arcade buttons on GPIO13/15 (live level + press count on the diagnostics
  screen), watchdog — builds clean for esp32s3. Team A moved off GPIO16:
  that pin is RS485_RXD, driven by the onboard SP3485 output, so a switch
  there fights a push-pull driver (`docs/HARDWARE_PINOUT.md`)
- `firmware/button-test` (DevKitC-1): arcade button/lamp bench harness —
  ISR-timestamped edges so bounce resolves to microseconds, per-button
  bounce/glitch/hold stats, boot lamp chase, verdict against the 30 ms
  debounce the product firmware assumes; builds clean for esp32s3
- `firmware/remote` also builds for esp32s3 via `sdkconfig.defaults.esp32s3`
  (button GPIO4 / lamp GPIO16 = button 1 of the bench harness), so a DevKit
  plus one arcade button is a stand-in clicker; the tracked c3 sdkconfig is
  untouched
- 192 native tests passing (domain, protocol, application, persistence,
  integration, remote, ui, common)
- ESP-IDF v5.4.4; all three firmware projects build

# In progress

- Arcade button bounce: first bench run done, switches read noisier than the
  30 ms the firmware assumes (`docs/HARDWARE_PINOUT.md`). Needs a re-run on a
  solid harness before the debounce constants are changed; the court unit's
  wired buttons also lack the remote's 700 ms retrigger guard

# Blocked (hardware not yet arrived)

- ESP-NOW 500-press soak on the DevKits (`firmware/espnow-linktest/README.md`)
- Waveshare 7B bring-up: vendor demo, then verify `board_7b.cpp` pins +
  timings (`docs/WAVESHARE_BRINGUP.md`, `docs/HARDWARE_PINOUT.md`)
- Remote physical acceptance: range, latency, battery; haptics (M6)
- Remote power, remaining spec 11.4 steps: light/modem sleep between points
  (step 2, extends match runtime) and real current measurement (step 4) —
  the 80-90 mA awake figure is still a datasheet estimate, not measured, so
  the ~5 h runtime and 7-day standby target are both unverified. Needs a USB
  power meter or an inline multimeter
- Remote sleep/wake 100-cycle soak (spec line 1630): deferred to a later
  bench session. Shorten `CONFIG_PADEL_REMOTE_SLEEP_TIMEOUT_S` and press the
  button once per cycle. One full cycle is proven (below); what is untested
  is repetition
- Encrypted ESP-NOW peers (keys scheme in `docs/PAIRING.md`)

# Next three tasks (hardware day)

1. DevKits: run the linktest soak, record applied/duplicate/latency here
I (91832) linktest: === burst complete ===
I (91832) linktest: [final] presses=500 accepted=402 dup_accepted=98 failed=0 attempts=623
I (91832) linktest: [final] press->ACK latency ms: min=2.5 avg=139.7 max=1963.2 (n=500)
I (91832) linktest: acceptance: receiver applied counter must equal accepted+dup_accepted unique presses (500)
I (91842) gpio: GPIO[0]| InputEn: 1| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0
2. 7B: vendor demo -> board profile verification -> flash court-display ->
   Setup screen -> touch + buzzer + arcade buttons
3. XIAO: flash remote, pair via the on-screen flow, run the M4 acceptance
   matrix physically (loss, reboot, conflict, duplicate press), then the
   hold-to-undo checks: 3 s hold takes back your own point, a hold after the
   opponents scored is refused, and one hold never removes two points
4. Arcade buttons: re-run the bench test with crimped/soldered connections —
   the first run (numbers in `docs/HARDWARE_PINOUT.md`) shows worst-case
   bounce of 25-74 ms against a 30 ms debounce assumption, on an improvised
   alligator-clip harness. Then decide the debounce constants and run button 1
   as a stand-in clicker

# Last verified commands

- `ctest --test-dir build` — 192/192 tests pass (2026-08-05, hold-to-undo)
- `./build/native/simulator/court-sim/court-sim --tour` — 12 screenshots
  incl. club picker/mix/standings
- `idf.py build` in `firmware/court-display` (esp32s3) — clean (2026-08-05,
  remote-undo beep + diagnostics row)
- `idf.py build` in `firmware/button-test` (esp32s3) — clean (2026-08-05)
- `idf.py -B build-s3 -D SDKCONFIG=sdkconfig.s3 build` in `firmware/remote`
  (esp32s3 DevKit stand-in) — clean (2026-08-05)
- `idf.py build` in `firmware/remote` (esp32c3) — clean (2026-08-05,
  hold-to-undo)
