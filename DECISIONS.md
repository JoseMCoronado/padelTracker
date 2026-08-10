# Architecture Decision Records

## ADR-0001: Target 1024x600 (Waveshare LCD-7B) only

Status: Accepted

Context: The master spec requires responsive layouts for both 800x480 (LCD-7)
and 1024x600 (LCD-7B). The purchased display is the 7-inch 1024x600 variant.

Decision: Design the UI for 1024x600 only, still using layout practices (flex,
percentages, design tokens) rather than hardcoded coordinates. The 800x480
requirement (spec section 20.15) is dropped per its own escape clause. Verify
the PCB/SKU label when the board arrives; revisit only if it is not a 7B.

Consequences: Half the UI test surface. A future 800x480 court unit would need
a layout pass but no architectural change.

## ADR-0002: Club mini-set mode is a first-class scoring preset

Status: Accepted

Context: The club's real format (rotation_examples/) is rounds of two
mini-sets: first to 3 games, golden point at 40-40 (WPT rule), teams reshuffled
between mini-sets, Top2/Bottom2 computed from wins then game differential.

Decision: The engine ships with a "club" preset: sets_to_win = 1,
games_to_win = 3, win_by_two_games = false, tiebreak_enabled = false,
GameRule::GoldenPoint. A match is one mini-set. The projection exposes games
won and game differential so players/organizers can compute Top2/Bottom2.

Consequences: The most common club match is a native config, not a workaround.
Rotation automation remains P1; P0 records what P1 will need.

## ADR-0003: Composite transitions from PointAwarded; milestones are projected facts

Status: Accepted

Context: Spec section 8.6 allows either explicit GameWon/SetWon/MatchWon events
or reproducibly projected facts, and requires the choice to be documented.

Decision: The journal stores only the facts that cannot be derived:
PointAwarded, ScoringActionUndone, lifecycle events, serving changes. The pure
reducer computes game/set/match completion as part of applying PointAwarded.
Game, set, and match milestones are projected from state (revision, set
history, winner), not stored as separate events.

Consequences: The journal is minimal and cannot self-contradict (no risk of a
PointAwarded without its GameWon). Consumers needing milestone notifications
diff successive states (e.g. completed_set_count changed). Undo is simple: skip
the compensated PointAwarded during replay and every downstream consequence
disappears.

## ADR-0004: Undo = compensating event + full journal replay

Status: Accepted

Context: Spec section 8.8 requires undo as a compensating event without
mutating history, and suggests replay as the simplest correct implementation.

Decision: UndoLastScoringAction appends ScoringActionUndone{undone_event_id}
referencing the latest non-compensated PointAwarded since the last match
boundary (MatchCreated/MatchReset). State is rebuilt by replaying the journal,
skipping compensated events. No snapshots until replay time is measured to be a
problem (padel journals are a few hundred events; replay is microseconds).

Consequences: Undo across game/set/match/tiebreak boundaries is automatically
correct — there is no inverse-transition code to get wrong. Match-winning
points can be undone, reopening the match.

## ADR-0005: Journal will start as a single append-only CRC-framed file

Status: Accepted, implemented (`components/persistence`)

Context: Spec section 13 describes journal + snapshots. Snapshots add rotation
and consistency complexity that is unjustified before replay cost is measured.

Decision: One append-only file of length-delimited, CRC-framed binary records;
full replay on boot; truncate-at-first-corrupt-record recovery. Snapshots are
deferred until measurements justify them.

Record layout (little-endian): magic "PJ", schema version, total record
length, event_id, match_id, state_revision, monotonic_ms, input source,
intent identity (remote_id/boot_id/sequence, zeroed for local sources),
event type + payload, CRC16/CCITT-FALSE trailer. Carrying the intent identity
in each point record lets boot recovery rebuild the dedup watermarks
(ADR-0007) by pure replay — no separate dedup snapshot file. Recovery
distinguishes a truncated tail (torn write) from a corrupt record and from an
unsupported (newer) schema version; in all cases it stops at the last valid
record and reports, never silently resets (spec section 12.2). The genesis
MatchCreated event is journaled too, so replay reproduces the match config
without relying on the compiled-in default.

Consequences: Simpler recovery logic to harden and power-loss-test in M7.
Journal grows without bound within a match; bounded in practice (a few
hundred records) and reset on new match.

## ADR-0008: Two-phase decide/commit engine API; ACK only after durable append

Status: Accepted

Context: Spec section 13.3 requires that Accepted is only ever ACKed after
the event is durable, and a storage failure must leave state unchanged with
no rollback path.

Decision: MatchEngine exposes decide(cmd) -> DecidedEvent (validation only,
no state change; carries the event id and revision it will get) and
commit(DecidedEvent) (append + apply). The application service runs:
validate -> dedup classify -> conflict guard -> decide -> durable journal
append+sync -> engine commit -> dedup record -> ACK. A failed append aborts
before commit: state, revision, and dedup watermarks are untouched, the
remote gets ErrorStorage, and its retry is processed as brand new once
storage recovers. handle(cmd) = decide + commit remains for the CLI
simulator and tests.

Consequences: Exactly-once accounting holds across storage failures and
power loss at any pipeline stage (proven by the fault-injection matrix in
tests/persistence/test_power_loss.cpp). The single-threaded application task
must not interleave another command between decide and commit (asserted).

## ADR-0009: Conflict guard = pending window, both rejected, organizer resolves

Status: Accepted

Context: Spec section 12.4: simultaneous opposing presses must not silently
double-score. Exactly one rally happened; the two presses disagree about who
won it.

Decision: The first valid press (remote or wired backup button) opens a
configurable pending window (default 250 ms, injected monotonic clock; 0
disables the guard = first-press-wins). No opposing press by expiry: the
parked press commits and ACKs normally — the window only delays the ACK, it
never re-orders it before durability. An opposing press inside the window
parks neither: both intents receive terminal RejectedConflict, and the UI is
asked to resolve (organizer picks team A / team B / cancel); the resolution
is journaled as a TouchscreenAdmin-sourced point. Organizer/touch inputs
bypass the guard. Conflicted identities are remembered in a small ring so a
lost-ACK retry is re-ACKed RejectedConflict instead of reopening a window
and scoring a point the organizer may have cancelled.

Consequences: Every scoring path gains up to window-length latency
(250 ms < the remote's 450 ms ACK timeout, so no spurious retries). A second
distinct same-team press during the window commits the parked press
immediately and parks the new one, preserving one-point-per-intent.

## ADR-0010: LVGL pinned to v8.4.0, full UI developed on a desktop simulator

Status: Accepted

Context: The Waveshare 7B vendor demo (our display bring-up gate, ADR-0001 /
docs/WAVESHARE_BRINGUP.md) ships against LVGL v8.4.0. Hardware has not
arrived, but the UI is the largest remaining body of P0 work and none of it
is hardware-dependent except panel/touch init.

Decision: Pin LVGL v8.4.0 (FetchContent in the native build; the firmware
project consumes the same version as a managed component). Configure LVGL
via `LV_CONF_SKIP` + compile definitions instead of maintaining a forked
`lv_conf.h` (color depth 16 to match the RGB565 panel, Montserrat
14/16/20/28/48). Build the entire UI as a hardware-independent component
(`components/ui`) rendered natively in an SDL2 window (`simulator/court-sim`)
with the real CourtService and a real file journal behind it. SDL2 is built
from source via FetchContent because the Homebrew SDL2 on the dev machine is
x86_64-only. The live point score uses a digits-only Montserrat 300 px font
(`components/ui/fonts`, glyphs 0-9/A/D) in a flex-grown slot so numerals fill
the team panel; match-complete banners still use Montserrat 48. On-device,
only the board profile (panel + GT911 + backlight) differs from the
simulator backend.

Consequences: All screens, flows, and UI checks run and regress natively; a
future LVGL 9 migration is deliberate and separate. Risk accepted: minor
rendering differences between SDL and the RGB panel are verified on
bring-up day.

## ADR-0011: Physical-presence pairing with organizer confirmation

Status: Accepted

Context: Spec 10.8 requires an explicit pairing step binding a remote to a
court + team, surviving reboots, with no silent remote replacement. The
radio (ESP-NOW) gives no identity guarantees by itself.

Decision: Two broadcast packets on top of the existing CRC16 framing:
`PAIR_REQUEST` (remote -> court, sent every 500 ms while the remote is in
pairing mode after a 5 s button hold) and `PAIR_ASSIGN` (court -> remote,
carrying court_id + team + channel, filtered by remote_id). Pairing only
proceeds inside an organizer-opened 30 s window for a specific team, and
the organizer must confirm the surfaced short device id. Assignments
persist via `ISettings` (NVS on device, file natively) and load into the
CourtService allow-list on boot. A remote assigned to the other team is
ignored until explicitly unassigned. Key provisioning (PMK/LMK) is
documented in docs/PAIRING.md; P0 runs unencrypted like the linktest, with
dev keys in a gitignored header.

Consequences: Pairing is testable natively end to end (remote_core ->
PairingService), and accidental cross-court binding requires physical
presence plus explicit confirmation. Encrypted peers are a bring-up task,
not a redesign.

## ADR-0012: Court firmware task split — radio callback / app task / LVGL task

Status: Accepted (verification on hardware)

Context: Spec 12.5/23.3 requires that the radio callback never blocks, that
flash writes never run on the UI task, and that queue overflow is a
visible fault.

Decision: In `firmware/court-display`: the ESP-NOW receive callback only
copies the frame into a bounded FreeRTOS queue (overflow counted and shown
in diagnostics). One application task owns CourtService, PairingService and
the journal: it drains the radio queue and a mutex-guarded UI command
queue, ticks the conflict window, transmits ACKs, and publishes immutable
`UiModel` snapshots. The LVGL task (pinned to the other core) renders
snapshots and handles touch; UI callbacks only enqueue commands. This
mirrors court-sim exactly, so behavior differences on hardware are
transport-only.

Consequences: No LVGL call ever waits on flash or radio; scoring latency is
bounded by the app task loop (10 ms) rather than rendering. Cost: one
model copy per render tick, acceptable at 1024x600/16bpp scale.

## ADR-0006: ESP-IDF v5.4.x line for bring-up

Status: Provisional (final pin in M2 per spec version policy)

Context: The spec pins toolchain versions to whatever the vendor example for
the exact board proves out. Hardware has not arrived; Waveshare ESP32-S3
examples have historically targeted ESP-IDF v5.3/v5.4.

Decision: Install ESP-IDF v5.4.4 now so bring-up is unblocked. When the exact
board model is confirmed and the official demo is built untouched (M2), record
and pin the proven versions in docs/TOOLCHAIN.md, switching IDF version if the
vendor example requires it.

Consequences: Possible one-time IDF version switch in M2; native core is
unaffected either way.

## ADR-0007: Deduplication watermark with bounded duplicate window

Status: Accepted

Context: Spec section 10.6 requires dedup by (remote_id, boot_id, sequence),
sequence-wrap handling, and persistable dedup state. The remote protocol is
stop-and-wait: a remote never issues a new sequence while an intent is pending,
so sequences from one remote arrive in order.

Decision: Per remote, store (boot_id, highest accepted sequence). Wrap-safe
signed serial arithmetic classifies an incoming sequence: ahead = New (advance
watermark), equal or within a 64-sequence window behind = Duplicate, further
behind = Stale (rejected). A new boot_id resets the entry (remotes generate a
random boot_id each boot). The watermark set is small, serializable, and will
be included in journal/snapshot state in M3.

Consequences: O(1) memory per remote, survives reboot via persistence, and a
retried packet after a court reboot is classified Duplicate, not applied twice.

## ADR-0013: Club round as a layer above the match engine, players by slot

Status: Accepted

Context: Club play (rotation sheets) needs per-court rounds of two
first-to-3 mini-sets with a fixed mix rule, per-player standings
(wins, then game differential), Top 2 / Bottom 2, and a tie broken by a
coin flip. The user wants named players from a roster (seed: Jose, Zoe,
William, Szewei, Ruxandra, Lewis, Luigi, Raymond, Paulina, Vineet, Louis,
Adrien), on-device player creation, unpersisted guests, and the program to
flip the coin itself and announce the result.

Decision: Keep the match engine untouched. `domain::ClubRound` is pure
logic over player slots 0..3: pairings (set 1 = picks, set 2 = winners
split and each takes a loser), standings, Top 2 / Bottom 2, and a
deterministic coin flip from an injected seed. Names live in
`application::PlayerRoster` (file-backed store, seeded on first run;
guests are minted per session with sentinel ids and never persisted).
`application::ClubController` maps players to slots, runs each mini-set as
a normal journaled match (club preset), consumes the completed
`MatchState`, and remembers the round's Top 2 as the forbidden pair for
the next round. Hosts (court-sim, court-display) only route: match
complete -> summary -> mix screen -> standings screen. The picker UI
enforces 2-per-team selection and creates players through the host round
trip.

The results log (one CSV row per player, the future stats/sync feed) is
written by `finish_round()` rather than the moment set 2 lands, because a
remote undo can reopen a finished mini-set (ADR-0014) and rows already on
disk would then describe a round that no longer happened.

**Barred pairs (amended after the first club session).** A court's Top 2
stay together for the next round but may never partner, and the mix rule
above happily recreates them: on the sheet's round 2 court 2, A&G beating
B&E turns into A&B vs G&E, exactly the pair the sheet forbids. The
winners-split has two legal forms, so `ClubRound` takes up to two
forbidden slot pairs and picks the form that avoids them (here A&E vs
B&G, which is what the sheet plays). Two pairs, not one: a court hosts
the Top 2 that stayed *and* the Top 2 that came up from the court below.
The organizer marks the second pair by double-tapping the names in the
picker, which cycles a crown badge (none, 1, 2); the previous round's own
Top 2 is crowned automatically when the NEW ROUND suggestion lands.
Crowned pairs are rejected as set-1 teammates and kept apart by the mix.

Consequences: Every club rule is natively tested against the rotation
sheet examples, including the coin-flip announcement; each mini-set gets
the full journal/recovery treatment for free. A power cycle mid-round
loses the round bookkeeping (a mini-set is minutes long); multi-court
rotation (M8) can consume Top2/Bottom2 without touching the scoring path.
Crowns are per-round UI state, not roster state: they describe where a
player came from tonight, not who they are.

## ADR-0014: Remote hold-to-undo, taking back the match's last point

Status: Accepted — deliberate departure from spec 11.2 and 14.6

Context: Spec 14.6 makes undo an organizer action ("no remote needs to be
involved") and 11.2 states that no remote gesture undoes anything; the
player role explicitly has "no destructive controls". In practice the
court unit is at the net post and the players are on court with the
remotes, so the only way to fix a mis-press today is to walk over and use
the touchscreen. The user asked for a remote gesture and chose a hold
over a double press.

A double press was rejected: the 700 ms retrigger guard exists precisely
because accidental double taps are the common failure mode of a clip-on
button, and the bench measurements on the arcade switches showed bounce
bursts long enough to fake one. Turning the most likely accident into a
destructive action inverts that safety property.

Decision: Holding a paired remote for `undo_hold_ms` sends a POINT_INTENT
carrying `Action::UndoLastPoint`, which the court applies as
`UndoLastScoringAction{}` journaled with `InputSource::Remote`. Two
constraints keep it from becoming a dispute generator:

- **Once per hold.** The gesture fires a single intent no matter how long
  the button stays down; a second undo needs a fresh press.
- **Refused while unsettled.** A hold arriving with a press parked in the
  conflict window, or with a conflict on screen, is rejected outright.

**Amended after the first on-court session.** Two of the original choices
did not survive contact with a real match:

- **1.5 s, not 3 s.** Three seconds of standing still with a thumb on the
  button is a long time between points; the hold is now `undo_hold_ms` =
  1500 ms, still double the 700 ms retrigger guard and a third of the
  pairing hold. The hold is timed from the physical button edge rather
  than from the moment the debounce accepts it, so tuning the debounce
  never stretches the gesture.
- **Global, not team-scoped.** The undo originally only reversed a point
  belonging to the holder's team, which meant a player who watched the
  wrong button get pressed could do nothing about it and got a rejection
  buzz for trying. Either remote now takes back whichever point came
  last. It still reaches back exactly one point, and the on-screen
  organizer undo already worked this way.

Undo crosses game, set and match boundaries because the engine replays
the journal rather than stepping the score back, so taking back a
match-winning point reopens the match. The hosts mirror that: when the
lifecycle leaves `Completed` while a post-match screen is up, they return
to Live and, in a club round, un-record the mini-set the controller had
already consumed.

The award intent therefore moves from press-down to release: while the
button is down, the press could still become a hold, and an undo that
fired *after* its own press had already scored would just cancel itself
and leave the mis-pressed point standing. The press cue still plays on
contact, so the remote feels unchanged; the added latency is the length
of the press. Reusing the POINT_INTENT frame rather than adding a message
type means the undo inherits sequence identity, dedup and retries, so a
lost ACK cannot remove two points.

Consequences: A player can silently reverse the last point with nobody at
the court unit, which is exactly what the spec was protecting against —
accepted knowingly, mitigated by a distinct 500 ms court beep so an undo
is never mistaken for a score, and by the fact that the score is on a
1024x600 display everyone can see. `undo_hold_ms` must stay above
`stable_press_ms` (150 ms, or no press could score) and below
`pairing_hold_ms` (5 s), which only matters while unpaired, where the
hold still means pairing and never sends an undo.

## ADR-0015: Remote deep sleep after inactivity, and the waking press never scores

Status: Accepted

Context: The remote runs always-awake with the ESP-NOW receiver on, which
measures out at tens of milliamps. On a 500 mAh LiPo that covers a session
comfortably but flattens the cell in well under a day of standby, so a
remote left in a kit bag is dead when it is next needed. Spec 11.4 orders
the power work as always-awake, then light/modem sleep, then deep sleep
after inactivity, gated on "do not optimize sleep before packet/ACK
reliability is tested". That gate was cleared on hardware: both units
paired, scored, deduplicated and survived power cycles.

Decision: Deep sleep after `inactivity_sleep_ms` (15 min), waking on the
point button. Two parts are worth recording.

**Step 3 before step 2.** Light sleep at roughly 130 uA would already beat
the 7-day standby target in spec 4.5, but standby is the failure mode that
actually bites, and deep sleep reaches it by a path that is already proven:
a wake is a reboot, and reboots are safe because `sequence_baseline` in NVS
only moves forward and the deduplicator treats a fresh `boot_id` as a new
sequence space. Light/modem sleep between points, which would extend
*match* runtime rather than standby, is still open as spec 11.4 step 2.

**The waking press does not score.** A wake takes a few hundred
milliseconds to re-init, and measured taps on the arcade switches run
190-320 ms, so the button is usually released before the firmware is
listening. Inferring a point from the wake cause was rejected: it would let
a remote jostled in a bag silently add a point, and a phantom point
corrupts a score in a way nobody can later reconstruct, which is worse than
a press that visibly did nothing. The firmware instead masks the button
until it is seen released, swallowing exactly the waking press, and plays a
distinct two-pulse `Woke` cue so the remote is visibly alive.

The decision of *when* to sleep lives in `RemoteCore::sleep_due()` rather
than the firmware, so it is covered by native tests. It refuses while an
intent is in flight, while advertising for a pairing, and while the button
is down, which is what stops sleep from stranding an unacknowledged point.
An unpaired remote does sleep: `PairingRequired` is otherwise a state a
remote can sit in forever, draining in a drawer.

Consequences: The first press of a session after a long gap wakes rather
than scores, which players must learn; the LED cue and the fact that
organizer setup happens on the touchscreen first make this cheap in
practice. A spurious wake costs `post_wake_idle_ms` (60 s) of awake time
rather than a full timeout. Each wake is a reboot, so it burns one NVS
baseline write and a 32-sequence chunk on the following press, which is
immaterial against NVS endurance. Deep sleep wake constrains the button to
GPIO0-5 on the ESP32-C3; a `static_assert` enforces this, since moving the
button to any other pad would otherwise silently produce a remote that
never wakes. Because the console is the chip's own USB Serial/JTAG, a
sleeping remote vanishes from the host's serial ports entirely, so
`enter_deep_sleep()` drains the console first or the remote appears to have
crashed; see `docs/HARDWARE_PINOUT.md`.

Verified on hardware 2026-08-05 with a 60 s bench timeout: slept on
schedule, woke on the D1 press, that press left `presses=0 intents=0`, and
the following press scored and was ACKed by the court in 490 ms. Repetition
(the 100-cycle soak in spec line 1630) is still outstanding.

## ADR-0016: A press is 150 ms of contact, not 30

Status: Accepted — supersedes the debounce parameters in spec 11.2

Context: The first real session on court produced phantom points. Players
wear the remote clipped on, and a shirt dragging across the button as
somebody turns is enough contact to score. Spec 11.2's initial
`stable_press_ms` of 30 ms was chosen to reject switch bounce, and the
bench measurements (`docs/HARDWARE_PINOUT.md`) show typical bounce of
1.6-4.8 ms with bursts to 74 ms, so 30 ms was already marginal against
bounce alone. Against fabric it is nowhere near enough.

Decision: A press must hold the button down for `stable_press_ms` = 150 ms
before it counts, on the remote and on the court unit's wired buttons
alike. Release stays at 30 ms: a press that has been accepted should end
promptly. The threshold is exposed as `PADEL_REMOTE_PRESS_MS` (range
30-600) so a court that still sees phantom points can be retuned without
a rebuild of anything but the remote.

150 ms was picked as the widest window that still feels instant. Measured
deliberate taps on the arcade switches run 190-320 ms (ADR-0015), so a
real press clears it with margin, while the brushes that caused the
problem are tens of milliseconds. It also comfortably swallows the 74 ms
worst-case bounce burst, which the old value did not.

Consequences: The `PressRegistered` cue now fires 150 ms after contact
rather than 30 ms, which is the only user-visible cost and reads as
instant. The scoring window is 150-1500 ms of hold, with anything longer
becoming an undo (ADR-0014); those two constants have to be read
together, and a Kconfig value above `undo_hold_ms` would make scoring
impossible, which is why the range caps at 600. Native tests now cover
brush-length contacts of 30-140 ms registering nothing.

## ADR-0017: Broadcast-style scoreboard and a match summary before the flow moves on

Status: Accepted

Context: The live screen carried the set history as one line of text in
the footer ("Set history: 7-6(5) 4-6 | current 5-6") plus a per-panel
"Games 5   Sets 1". From the far side of a court, that is unreadable —
which the first session confirmed. Players also wanted to look back at a
match once it finished, and in club play the flow jumped straight from
the last point to the mix or standings screen with nothing in between.

Decision: Two changes, sharing one widget.

The footer becomes a 132 px band holding a scoreboard laid out like the
pro tour overlays: one row per team, a team-colored name plate with a
serve dot, then one fixed-width cell per set, the set in progress lit and
a lost set dimmed, the loser's tiebreak points in brackets. The per-panel
games/sets text and the footer history line are gone; the giant point
digits are untouched. `DisplayState` gained a structured `sets` vector so
the UI builds columns from data rather than parsing the display strings.

Completion routes through a new `Screen::MatchSummary` — winner, the same
scoreboard, duration, points won per side with percentages, longest run —
and its single CONTINUE button leads to the club mix, the club standings
or the ordinary complete screen. Rally counts come from walking the
journal and skipping compensated events, since `MatchState` only carries
the current score.

Consequences: The centre row shrinks from ~452 to ~376 px, still far more
than the 216 px score line needs. Five set columns is the widest board
the domain can produce and it fits, which the render test pins. The
summary is one more tap in every club round; it is also the only place
the round is reviewable, and it is where an undo lands when it reopens a
finished match.

## ADR-0018: The court unit speaks in pitches, from a shared cue table

Status: Accepted

Context: The buzzer was an active sounder on a plain GPIO, held high for a
fixed time, so the four things the court unit says differed only in length:
80 ms for an accepted point, 150 ms for pairing, 400 ms for a completed
match, 500 ms for a remote undo. Length is the one dimension a listener
cannot use. From the far side of a court, through glass and over the noise
of a club night, 80 ms and 150 ms of the same note are the same sound, and
the undo cue — the one event that happens with nobody near the unit, where
being noticed is the entire point — was distinguishable only by being
slightly longer than a score. The sounds also lived as bare `beep(ms)`
calls in `main.cpp`, which the spec's "feedback patterns MUST be
centralized in one module" rules out, and which left court-sim with a stub
that logged "BEEP" and proved nothing.

Decision: Drive a **passive** element with LEDC and give every cue its own
pitch shape. A point rises two notes, a remote undo falls two low ones,
pairing climbs three, a finished match plays a short fanfare. The patterns
are data in a portable `components/sound`, so the firmware, the simulator
and the native tests all read the same table; `main/buzzer.cpp` is the only
code that touches the pin. `PADEL_COURT_BUZZER_PASSIVE=n` keeps an active
buzzer working by playing the same patterns as rhythm with the pitch
dropped, which is the fallback if the passive element cannot be made loud
enough in a hall.

Every tone sits between 1 and 5 kHz, and the diagnostics test became a
stepped sweep of that band rather than a single beep. Both follow from the
same physical fact: a piezo is loud only near its mechanical resonance and
falls off steeply either side, so the band is where a sounder can carry at
all, and the sweep is how bring-up finds the resonance by ear.

The first cut put the undo an octave below a point, on the theory that low
and falling reads as "taken away". Hardware disagreed: on the element we
fitted, the sweep peaked near the top of the band, and 1047 Hz was audibly
feeble where 4186 Hz carried. Both remote-triggered cues therefore sit at
the top of the band, and the undo separates itself by shape instead — a
three-step descending run against a two-step rise, five times as long.
Pitch is what makes a cue audible here; contour and length are what make it
identifiable.

Consequences: Cues got longer — a point is 125 ms against the old 80, a
completed match 710 against 400 — which is affordable because rallies are
seconds apart, and a native test pins the point cue under 200 ms so
back-to-back points cannot swallow each other. Tone choice is now a design
surface that can be got wrong, so the tests assert the properties that
matter rather than the numbers: no two cues share a pitch sequence, a point
rises, an undo falls, and the undo has more steps and runs at least three
times longer. This does not make the unit capable of music. A piezo's volume
varies wildly with pitch, so melodies come out uneven and thin; real audio
would mean an I2S DAC, an amplifier and a speaker, and the 7B has no pins
left for it.
