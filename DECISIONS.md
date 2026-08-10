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
`MatchState`, appends one CSV row per player to a results log (the future
stats/sync feed), and remembers the round's Top 2 as the forbidden pair
for the next round. Hosts (court-sim, court-display) only route: match
complete -> mix screen -> standings screen. The picker UI enforces
2-per-team selection and creates players through the host round trip.

Consequences: Every club rule is natively tested against the rotation
sheet examples, including the coin-flip announcement; each mini-set gets
the full journal/recovery treatment for free. A power cycle mid-round
loses the round bookkeeping (a mini-set is minutes long); multi-court
rotation (M8) can consume Top2/Bottom2 without touching the scoring path.

## ADR-0014: Remote hold-to-undo, scoped to the holder's own last point

Status: Accepted — deliberate departure from spec 11.2 and 14.6

Context: Spec 14.6 makes undo an organizer action ("no remote needs to be
involved") and 11.2 states that no remote gesture undoes anything; the
player role explicitly has "no destructive controls". In practice the
court unit is at the net post and the players are on court with the
remotes, so the only way to fix a mis-press today is to walk over and use
the touchscreen. The user asked for a remote gesture and chose a 3 s hold
over a double press.

A double press was rejected: the 700 ms retrigger guard exists precisely
because accidental double taps are the common failure mode of a clip-on
button, and the bench measurements on the arcade switches showed bounce
bursts long enough to fake one. Turning the most likely accident into a
destructive action inverts that safety property.

Decision: Holding a paired remote for `undo_hold_ms` (3 s) sends a
POINT_INTENT carrying `Action::UndoLastPoint`, which the court applies as
`UndoLastScoringAction{only_team = the remote's team}` journaled with
`InputSource::Remote`. Three constraints keep it from becoming a dispute
generator:

- **Team-scoped.** The undo only proceeds if the *newest* point belongs to
  the holder's team. It never reverses the opponents' point and never
  reaches past one, so a rejected hold leaves the score exactly as it was.
- **Once per hold.** The gesture fires a single intent no matter how long
  the button stays down; a second undo needs a fresh press.
- **Refused while unsettled.** A hold arriving with a press parked in the
  conflict window, or with a conflict on screen, is rejected outright.

The award intent therefore moves from press-down to release: while the
button is down, the press could still become a hold, and an undo that
fired *after* its own press had already scored would just cancel itself
and leave the mis-pressed point standing. The press cue still plays on
contact, so the remote feels unchanged; the added latency is the length
of the press. Reusing the POINT_INTENT frame rather than adding a message
type means the undo inherits sequence identity, dedup and retries, so a
lost ACK cannot remove two points.

Consequences: A player can silently reverse their own last point with
nobody at the court unit, which is exactly what the spec was protecting
against — accepted knowingly, mitigated by team scoping and a distinct
500 ms court beep so an undo is never mistaken for a score. The organizer
undo on the Live screen is unchanged and still unscoped. `undo_hold_ms`
must stay below `pairing_hold_ms` (5 s), which only matters while
unpaired, where the hold still means pairing and never sends an undo.

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
