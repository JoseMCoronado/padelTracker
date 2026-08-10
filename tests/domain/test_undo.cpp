#include <catch2/catch_test_macros.hpp>

#include "domain/helpers.hpp"

using namespace padel;
using namespace padel::domain;
using namespace padel::domain::testing;

namespace {

void undo(MatchEngine& engine) {
    REQUIRE(engine.handle(UndoLastScoringAction{}).has_value());
}

}  // namespace

TEST_CASE("undo: ordinary point", "[undo]") {
    MatchEngine engine = started(preset_standard_advantage());
    award(engine, TeamId::A, 2);
    REQUIRE(display(engine).points_a == "30");

    undo(engine);
    REQUIRE(display(engine).points_a == "15");
    REQUIRE(display(engine).points_b == "0");
}

TEST_CASE("undo: game-winning point restores 40-30", "[undo]") {
    MatchEngine engine = started(preset_standard_advantage());
    award(engine, TeamId::A, 3);
    award(engine, TeamId::B, 2);
    award(engine, TeamId::A);  // game A
    REQUIRE(display(engine).games_a == 1);

    undo(engine);
    const DisplayState d = display(engine);
    REQUIRE(d.games_a == 0);
    REQUIRE(d.points_a == "40");
    REQUIRE(d.points_b == "30");
}

TEST_CASE("undo: game-winning point restores serving team", "[undo][serving]") {
    MatchEngine engine = started(preset_standard_advantage(), TeamId::A);
    win_game(engine, TeamId::A);
    REQUIRE(engine.state().serving_team == TeamId::B);

    undo(engine);
    REQUIRE(engine.state().serving_team == TeamId::A);
    REQUIRE(display(engine).points_a == "40");
}

TEST_CASE("undo: set-winning point restores the set in progress", "[undo]") {
    MatchEngine engine = started(preset_standard_advantage());
    reach_games(engine, 6, 4);
    REQUIRE(engine.state().sets_won_a == 1);

    undo(engine);
    const MatchState& s = engine.state();
    REQUIRE(s.sets_won_a == 0);
    REQUIRE(s.completed_set_count == 0);
    REQUIRE(s.current_set.games_a == 5);
    REQUIRE(s.current_set.games_b == 4);
    REQUIRE(display(engine).points_a == "40");
}

TEST_CASE("undo: match-winning point reopens the match", "[undo]") {
    MatchEngine engine = started(preset_standard_advantage());
    win_games(engine, TeamId::A, 6);
    win_games(engine, TeamId::A, 6);
    REQUIRE(engine.state().lifecycle == MatchLifecycle::Completed);
    REQUIRE(engine.state().winner == TeamId::A);

    undo(engine);
    const MatchState& s = engine.state();
    REQUIRE(s.lifecycle == MatchLifecycle::Active);
    REQUIRE_FALSE(s.winner.has_value());
    REQUIRE(s.sets_won_a == 1);
    REQUIRE(s.current_set.games_a == 5);

    // Scoring continues on the reopened match.
    award(engine, TeamId::B);
    REQUIRE(display(engine).points_b == "15");
}

TEST_CASE("undo: tiebreak point", "[undo][tiebreak]") {
    MatchEngine engine = started(preset_standard_advantage());
    reach_games(engine, 6, 6);
    award(engine, TeamId::A, 3);
    award(engine, TeamId::B, 1);
    REQUIRE(display(engine).points_a == "3");
    REQUIRE(display(engine).points_b == "1");

    undo(engine);
    REQUIRE(display(engine).points_a == "3");
    REQUIRE(display(engine).points_b == "0");
    REQUIRE(engine.state().in_tiebreak);
}

TEST_CASE("undo: tiebreak-winning point restores the tiebreak", "[undo][tiebreak]") {
    MatchEngine engine = started(preset_standard_advantage());
    reach_games(engine, 6, 6);
    award(engine, TeamId::A, 7);  // tiebreak 7-0, set 7-6
    REQUIRE(engine.state().sets_won_a == 1);

    undo(engine);
    const MatchState& s = engine.state();
    REQUIRE(s.sets_won_a == 0);
    REQUIRE(s.in_tiebreak);
    REQUIRE(s.tiebreak_points_a == 6);
    REQUIRE(s.tiebreak_points_b == 0);
}

TEST_CASE("undo: golden point", "[undo][golden]") {
    MatchEngine engine = started(preset_standard_golden_point());
    award(engine, TeamId::A, 3);
    award(engine, TeamId::B, 3);
    award(engine, TeamId::B);  // golden point -> game B
    REQUIRE(display(engine).games_b == 1);

    undo(engine);
    const DisplayState d = display(engine);
    REQUIRE(d.games_b == 0);
    REQUIRE(d.is_golden_point);
}

TEST_CASE("undo: multiple sequential undos walk backward", "[undo]") {
    MatchEngine engine = started(preset_standard_advantage());
    award(engine, TeamId::A, 2);
    award(engine, TeamId::B, 1);

    undo(engine);  // removes B point
    REQUIRE(display(engine).points_b == "0");
    REQUIRE(display(engine).points_a == "30");

    undo(engine);  // removes second A point
    REQUIRE(display(engine).points_a == "15");

    undo(engine);  // removes first A point
    REQUIRE(display(engine).points_a == "0");

    const auto exhausted = engine.handle(UndoLastScoringAction{});
    REQUIRE_FALSE(exhausted.has_value());
    REQUIRE(exhausted.error() == CommandError::NothingToUndo);
}

TEST_CASE("undo: followed by a new branch of scoring", "[undo]") {
    MatchEngine engine = started(preset_standard_advantage());
    award(engine, TeamId::A, 3);
    undo(engine);  // back to 30-0
    award(engine, TeamId::B, 2);

    const DisplayState d = display(engine);
    REQUIRE(d.points_a == "30");
    REQUIRE(d.points_b == "30");

    // The undone event stays in the journal; only its effect is compensated.
    REQUIRE(engine.journal().size() == 8);  // create+start+3a+undo+2b
}

TEST_CASE("undo: cannot cross a match reset boundary", "[undo][lifecycle]") {
    MatchEngine engine = started(preset_standard_advantage());
    award(engine, TeamId::A, 2);
    REQUIRE(engine.handle(ResetMatch{}).has_value());
    engine.handle(StartMatch{TeamId::A});

    const auto result = engine.handle(UndoLastScoringAction{});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == CommandError::NothingToUndo);
}

TEST_CASE("undo: next_undo_target previews the compensated point", "[undo]") {
    MatchEngine engine = started(preset_standard_advantage());
    REQUIRE_FALSE(engine.next_undo_target().has_value());

    award(engine, TeamId::B);
    const auto target = engine.next_undo_target();
    REQUIRE(target.has_value());
    REQUIRE(target->team == TeamId::B);
}

TEST_CASE("undo: team-scoped undo takes back that team's own point", "[undo][team]") {
    MatchEngine engine = started(preset_standard_advantage());
    award(engine, TeamId::A, 2);

    REQUIRE(engine.handle(UndoLastScoringAction{InputSource::Remote, TeamId::A}).has_value());
    REQUIRE(display(engine).points_a == "15");
}

TEST_CASE("undo: team-scoped undo refuses the opponents' point", "[undo][team]") {
    MatchEngine engine = started(preset_standard_advantage());
    award(engine, TeamId::A, 2);
    award(engine, TeamId::B);

    // Team A holding its remote must not reverse B's point, and must not
    // reach past it to A's earlier one either.
    const auto result = engine.handle(UndoLastScoringAction{InputSource::Remote, TeamId::A});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == CommandError::NothingToUndo);
    REQUIRE(display(engine).points_a == "30");
    REQUIRE(display(engine).points_b == "15");
}

TEST_CASE("undo: team-scoped undo stops after the team's point is gone", "[undo][team]") {
    MatchEngine engine = started(preset_standard_advantage());
    award(engine, TeamId::A, 2);
    REQUIRE(engine.handle(UndoLastScoringAction{InputSource::Remote, TeamId::A}).has_value());

    // The next-newest point is A's too, so a second hold is allowed to take
    // that one back as well.
    REQUIRE(engine.handle(UndoLastScoringAction{InputSource::Remote, TeamId::A}).has_value());
    REQUIRE(display(engine).points_a == "0");

    const auto third = engine.handle(UndoLastScoringAction{InputSource::Remote, TeamId::A});
    REQUIRE_FALSE(third.has_value());
    REQUIRE(third.error() == CommandError::NothingToUndo);
}

TEST_CASE("undo: state matches never-happened history (revision aside)", "[undo][replay]") {
    // Undo must produce the same scoreboard as if the point never happened.
    MatchEngine with_undo = started(preset_standard_advantage());
    award(with_undo, TeamId::A, 3);
    award(with_undo, TeamId::B, 2);
    award(with_undo, TeamId::A);  // game
    with_undo.handle(UndoLastScoringAction{});

    MatchEngine clean = started(preset_standard_advantage());
    award(clean, TeamId::A, 3);
    award(clean, TeamId::B, 2);

    const DisplayState a = display(with_undo);
    const DisplayState b = display(clean);
    REQUIRE(a.points_a == b.points_a);
    REQUIRE(a.points_b == b.points_b);
    REQUIRE(a.games_a == b.games_a);
    REQUIRE(a.games_b == b.games_b);
    REQUIRE(with_undo.state().serving_team == clean.state().serving_team);
}
