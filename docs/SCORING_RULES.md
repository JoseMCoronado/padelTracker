# Scoring rules

The engine represents scoring format as data (`MatchConfig`), not scattered
conditionals. Raw point counts are stored internally; `0/15/30/40/DEUCE/AD/
GOLDEN POINT` are display projections.

## Game rules

### Advantage (traditional)

- Raw 0/1/2/3 display as 0/15/30/40.
- Both teams at >= 3 raw points: equal = DEUCE, one-point lead = AD.
- Game won at >= 4 raw points with a lead of >= 2.

### Golden point (WPT 40-40 rule)

- At 40-40 the next point wins the game; the display shows GOLDEN POINT.
- Equivalent formulation: first team to 4 raw points wins the game.
- The WPT detail that the receiving team chooses the receiving side is a
  physical/on-court rule; the engine only needs the golden-point win condition.

## Set rules (`SetRule`)

Configurable: `games_to_win`, `win_by_two_games`, `tiebreak_enabled`,
`tiebreak_at_games`, `tiebreak_points_to_win`, `tiebreak_win_by_two`.

Standard set: first to 6 games, win by two, tiebreak at 6-6 (first to 7 points,
win by two); set recorded as 7-6 with tiebreak points.

## Match rules (`MatchConfig`)

`sets_to_win` (default 2 = best of three), `final_set_rule`:

- `FullSet`: deciding set uses the normal set rule.
- `MatchTiebreak`: deciding set is a single tiebreak to
  `match_tiebreak_points_to_win` (default 10), recorded as a 1-0 set with
  tiebreak points.

## Presets

| Preset | Game rule | Set | Match |
|---|---|---|---|
| `standard` | Advantage | 6 games, TB at 6-6 | best of 3 |
| `golden` | Golden point | 6 games, TB at 6-6 | best of 3 |
| `club` | Golden point | first to 3 games, no win-by-two, no TB | single set |
| `tiebreak-final` | Advantage | 6 games, TB at 6-6 | best of 3, final = match TB to 10 |

## Club format (from rotation_examples/)

Round structure (organizer-level, P1 automation later):

1. Play a mini-set: first to 3 games, golden point (`club` preset).
2. Mix teams within the court, play a second mini-set. The set 1 winners split
   up and each takes a loser, avoiding any barred pair (see below).
3. Top 2 of the court = the player with 2 wins plus the 1-win player with the
   better game differential (3-0 beats 3-2). Ties: coin flip.
4. Two-court rotation: Court 2 Top 2 stay, Bottom 2 to Court 3; Court 3 Top 2
   up. Three-court: middle court is a transition court; when all flags are up
   everyone shifts together. The Top 2 of a court cannot partner each other in
   the following match.

### Barred pairs (crowns)

Rule 4's last sentence binds both mini-sets, not just the picks. Two players
who were a Top 2 together cannot be teammates in set 1, and the set-2 mix must
choose the winners-split that keeps them apart — a court hosts up to two such
pairs, the Top 2 that stayed and the Top 2 that came up.

On the setup screen the previous round's Top 2 is barred automatically. The
pair arriving from another court is marked by the organizer: double-tapping a
name in the player picker cycles a crown badge (none, 1, 2), and two players
wearing the same crown are barred. Picking a barred pair as teammates is
rejected with a hint before the round starts.

P0 obligation: the engine and results screen must expose per-mini-set final
games and game differential so Top2/Bottom2 can be computed manually. The
rotation policy itself lives in the future coordinator (spec section 21.4).

## Serving

P0 tracks the serving **team** only. Serving alternates at game boundaries and
can be set explicitly by the organizer. Within a tiebreak the engine does not
model the internal serve rotation (documented limitation, spec section 9.6).

## Undo

Undo appends a compensating `ScoringActionUndone` event referencing the undone
`PointAwarded`; state is rebuilt by replay. Works across every boundary: plain
point, game-winning, set-winning, match-winning (reopens the match), tiebreak,
and golden point. Multiple sequential undos walk backward through history.

It always takes back the match's most recent point, whichever team scored it,
whether it comes from the organizer menu or from a 1.5 s hold on either remote
(ADR-0014). Undoing a match-winning point returns the display to the live
screen; in a club round it also puts the finished mini-set back into play.
