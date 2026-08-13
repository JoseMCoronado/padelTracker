# Current milestone

M0/M1 — Repository, scoring engine, simulator: COMPLETE
M2/M3 prep — Application layer, persistence, radio simulation: COMPLETE (native)
M3–M7 software — UI, remote core, pairing, both firmware apps, observability:
COMPLETE (native + firmware builds; hardware verification pending)
First on-court session played 2026-08-10; the five fixes it produced (button
sensitivity, faster global undo, broadcast scoreboard, match summary, club mix
barred pairs) are in — see "From the first court session" below
Next: reflash both units and replay the session

# Working

- Native domain scoring engine (advantage, golden point, sets, tiebreaks,
  match tiebreak, club mini-set preset) — `components/domain`
- Undo via compensating events + journal replay, across all boundaries;
  always takes back the match's last point whoever scored it, and undoing a
  match-winning point reopens the match and the club mini-set with it
- Protocol: POINT_INTENT/ACK + PAIR_REQUEST/PAIR_ASSIGN with CRC16, golden
  vectors, wrap-safe deduplicator — `components/protocol`
- Application layer — `components/application`: CourtService single entry
  point, conflict guard (ADR-0009), PairingService with persisted
  allow-list (ADR-0011)
- Persistence — `components/persistence`: CRC-framed journal, recovery,
  power-loss fault matrix green (ADR-0005)
- Remote core — `components/remote_core`: portable state machine (debounce
  150/30/700 ms, stop-and-wait retries, feedback table, NVS sequence
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
- Remote hold-to-undo (ADR-0014): a 1.5 s hold on either remote takes back the
  match's last point via `Action::UndoLastPoint` on the POINT_INTENT frame,
  timed from the physical button edge. Award intents go out on button release
  rather than press-down so a hold can still become an undo; the court answers
  with a long descending cue so it is never mistaken for a score. A deliberate
  departure from spec 11.2/14.6 — see the ADR
- Full LVGL v8.4.0 UI — `components/ui` (ADR-0010): setup, live match,
  match summary, match complete, undo preview, protected reset, pairing,
  diagnostics, recovery; view models projected from CourtService; headless
  render tests at 1024x600 with stress content (spec 18.6 subset)
- Broadcast-style scoreboard (ADR-0017, ADR-0021): the live screen's bottom
  band and the summary share one widget — team name plate with a serve dot,
  one cell per set, current set lit, loser's tiebreak points in brackets.
  Pair labels are cut to three capitals a side ("JOS/RUX"), and a club round
  shows a block per mini-set so the set the mix swapped partners out of keeps
  its own names
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
  on device, session guests) + CSV results log written when the round is
  closed; touch player picker (search, tiles, NEW PLAYER, ADD GUEST,
  double-tap crowns), mix + standings screens; wired into both court-sim and
  court-display, with up to two barred pairs kept apart in the picks and in
  the set-2 mix
- `firmware/remote` (XIAO ESP32-C3): remote_core + ESP-NOW + button + LED +
  NVS — builds clean for esp32c3. Fixed on hardware 2026-08-05: the main
  loop's `vTaskDelay(pdMS_TO_TICKS(5))` rounded to zero ticks at the default
  100 Hz tick, so the task never blocked and the idle task starved until the
  watchdog fired every 5 s. Delay is now clamped to at least one tick; this
  affected the esp32c3 build identically
- `firmware/court-display` (Waveshare 7B): board profile (unverified pins/
  timings isolated in `board_7b.cpp`), radio-callback -> queue -> app task ->
  LVGL task split (ADR-0012), LittleFS journal + NVS settings, LEDC tone
  buzzer on the sensor-header GPIO6 (per-cue pitch shapes from the portable
  `components/sound`, ADR-0018) + arcade buttons on GPIO13/15 (live level +
  press count on the diagnostics screen), watchdog — builds clean for
  esp32s3. Team A moved off GPIO16:
  that pin is RS485_RXD, driven by the onboard SP3485 output, so a switch
  there fights a push-pull driver (`docs/HARDWARE_PINOUT.md`)
- `firmware/button-test` (DevKitC-1): arcade button/lamp bench harness —
  ISR-timestamped edges so bounce resolves to microseconds, per-button
  bounce/glitch/hold stats, boot lamp chase, verdict against the bounce
  threshold the product firmware assumes; builds clean for esp32s3
- `firmware/remote` also builds for esp32s3 via `sdkconfig.defaults.esp32s3`
  (button GPIO4 / lamp GPIO16 = button 1 of the bench harness), so a DevKit
  plus one arcade button is a stand-in clicker; the tracked c3 sdkconfig is
  untouched
- 223 native tests passing (domain, protocol, application, persistence,
  integration, remote, ui, common, sound)
- ESP-IDF v5.4.4; all three firmware projects build

# From the first court session (2026-08-10)

Five things the session produced, all landed:

1. **Phantom points from clothing.** A press now needs 150 ms of contact
   instead of 30 (ADR-0016), on the remotes and the court unit's wired
   buttons; `CONFIG_PADEL_REMOTE_PRESS_MS` retunes it courtside
2. **Undo took too long and refused too much.** 1.5 s instead of 3 s, and
   either remote takes back whichever point came last (ADR-0014)
3. **The games/sets strip was unreadable.** Replaced by the broadcast-style
   scoreboard (ADR-0017)
4. **No way to review a match.** New summary screen between the last point
   and whatever comes next (ADR-0017)
5. **The club mix recreated the forbidden Top 2 pair.** The mix now picks the
   winners-split that avoids barred pairs, and crowns let the organizer mark
   the pair that came up from another court (ADR-0013 amendment)

# In progress

- Arcade button bounce: first bench run done, switches read noisier than the
  30 ms originally assumed (`docs/HARDWARE_PINOUT.md`); the 150 ms press
  threshold now covers the worst-case 74 ms burst, but a re-run on a solid
  harness is still wanted. The court unit's wired buttons also lack the
  remote's 700 ms retrigger guard

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
   hold-to-undo checks: a 1.5 s hold takes back the last point whichever
   remote holds, and one hold never removes two points
4. Arcade buttons: re-run the bench test with crimped/soldered connections —
   the first run (numbers in `docs/HARDWARE_PINOUT.md`) shows worst-case
   bounce of 25-74 ms on an improvised alligator-clip harness, which the new
   150 ms press threshold covers; confirm it, then run button 1 as a stand-in
   clicker
5. Court session replay: with both units flashed, check that a shirt across a
   button scores nothing and that a full club round survives an undo taken
   from the summary screen

# Last verified commands

- `ctest --test-dir build/native` — 248/248 tests pass (2026-08-13, club
  scoreboard strip)
- `./build/native/simulator/court-sim/court-sim --tour` — 17 screenshots
  incl. club picker with crowns, mix, standings, match summary, and a club
  set 2 footer carrying both mini-sets
- `idf.py build` in `firmware/court-display` (esp32s3) — clean (2026-08-13,
  club scoreboard strip)
- `idf.py build` in `firmware/button-test` (esp32s3) — clean (2026-08-10)
- `idf.py -B build-s3 -D SDKCONFIG=sdkconfig.s3 build` in `firmware/remote`
  (esp32s3 DevKit stand-in) — clean (2026-08-10) once deep sleep is off: the
  wake path is the C3's GPIO one, so `sdkconfig.defaults.esp32s3` now sets
  `CONFIG_PADEL_REMOTE_SLEEP_ENABLE=n` and the code says so if you re-enable it
- `idf.py build` in `firmware/remote` (esp32c3) — clean (2026-08-10, 150 ms
  press + 1.5 s undo)
