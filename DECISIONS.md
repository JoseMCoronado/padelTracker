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

Status: Accepted (implementation lands in M3 persistence work)

Context: Spec section 13 describes journal + snapshots. Snapshots add rotation
and consistency complexity that is unjustified before replay cost is measured.

Decision: One append-only file of length-delimited, CRC-framed binary records;
full replay on boot; truncate-at-first-corrupt-record recovery. Snapshots are
deferred until measurements justify them.

Consequences: Simpler recovery logic to harden and power-loss-test in M7.

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
