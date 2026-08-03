#include <catch2/catch_test_macros.hpp>

#include "domain/helpers.hpp"

using namespace padel;
using namespace padel::domain;
using namespace padel::domain::testing;

TEST_CASE("set: 6-0", "[sets]") {
    MatchEngine engine = started(preset_standard_advantage());
    win_games(engine, TeamId::A, 6);

    const MatchState& s = engine.state();
    REQUIRE(s.sets_won_a == 1);
    REQUIRE(s.completed_set_count == 1);
    REQUIRE(s.completed_sets[0].games_a == 6);
    REQUIRE(s.completed_sets[0].games_b == 0);
    REQUIRE(display(engine).set_history.front() == "6-0");
}

TEST_CASE("set: 6-4", "[sets]") {
    MatchEngine engine = started(preset_standard_advantage());
    reach_games(engine, 6, 4);

    const MatchState& s = engine.state();
    REQUIRE(s.sets_won_a == 1);
    REQUIRE(s.completed_sets[0].games_a == 6);
    REQUIRE(s.completed_sets[0].games_b == 4);
}

TEST_CASE("set: 6-5 does not win; 7-5 does", "[sets]") {
    MatchEngine engine = started(preset_standard_advantage());
    reach_games(engine, 5, 5);
    win_game(engine, TeamId::A);  // 6-5
    REQUIRE(engine.state().sets_won_a == 0);
    REQUIRE(engine.state().current_set.games_a == 6);

    win_game(engine, TeamId::A);  // 7-5
    const MatchState& s = engine.state();
    REQUIRE(s.sets_won_a == 1);
    REQUIRE(s.completed_sets[0].games_a == 7);
    REQUIRE(s.completed_sets[0].games_b == 5);
}

TEST_CASE("set: 6-6 enters tiebreak", "[sets][tiebreak]") {
    MatchEngine engine = started(preset_standard_advantage());
    reach_games(engine, 6, 6);

    REQUIRE(engine.state().in_tiebreak);
    REQUIRE_FALSE(engine.state().in_match_tiebreak);
    const DisplayState d = display(engine);
    REQUIRE(d.is_tiebreak);
    REQUIRE(d.points_a == "0");
    REQUIRE(d.points_b == "0");
}

TEST_CASE("tiebreak: 7-0 wins set 7-6", "[sets][tiebreak]") {
    MatchEngine engine = started(preset_standard_advantage());
    reach_games(engine, 6, 6);
    award(engine, TeamId::A, 7);

    const MatchState& s = engine.state();
    REQUIRE(s.sets_won_a == 1);
    REQUIRE(s.completed_sets[0].games_a == 7);
    REQUIRE(s.completed_sets[0].games_b == 6);
    REQUIRE(s.completed_sets[0].tiebreak_points_a.value() == 7);
    REQUIRE(s.completed_sets[0].tiebreak_points_b.value() == 0);
    REQUIRE(display(engine).set_history.front() == "7-6(0)");
}

TEST_CASE("tiebreak: win by two required (6-6 -> 8-6)", "[sets][tiebreak]") {
    MatchEngine engine = started(preset_standard_advantage());
    reach_games(engine, 6, 6);

    // Alternate to 6-6 in the tiebreak.
    for (int i = 0; i < 6; ++i) {
        award(engine, TeamId::A);
        award(engine, TeamId::B);
    }
    REQUIRE(engine.state().in_tiebreak);

    award(engine, TeamId::A);  // 7-6: not enough
    REQUIRE(engine.state().in_tiebreak);
    REQUIRE(engine.state().sets_won_a == 0);

    award(engine, TeamId::A);  // 8-6: set won
    const MatchState& s = engine.state();
    REQUIRE(s.sets_won_a == 1);
    REQUIRE(s.completed_sets[0].tiebreak_points_a.value() == 8);
    REQUIRE(s.completed_sets[0].tiebreak_points_b.value() == 6);
    REQUIRE(display(engine).set_history.front() == "7-6(6)");
}

TEST_CASE("club mini-set: 3-0 wins the match", "[sets][club]") {
    MatchEngine engine = started(preset_club_mini_set());
    win_games(engine, TeamId::A, 3);

    const MatchState& s = engine.state();
    REQUIRE(s.lifecycle == MatchLifecycle::Completed);
    REQUIRE(s.winner == TeamId::A);
    REQUIRE(display(engine).game_differential == 3);
}

TEST_CASE("club mini-set: 3-2 wins without win-by-two", "[sets][club]") {
    MatchEngine engine = started(preset_club_mini_set());
    reach_games(engine, 2, 2);
    win_game(engine, TeamId::B);  // 2-3: B reaches 3 games, no win-by-two needed

    const MatchState& s = engine.state();
    REQUIRE(s.lifecycle == MatchLifecycle::Completed);
    REQUIRE(s.winner == TeamId::B);
    REQUIRE(display(engine).game_differential == -1);
}

TEST_CASE("club mini-set: golden point applies inside mini-set games", "[sets][club]") {
    MatchEngine engine = started(preset_club_mini_set());
    award(engine, TeamId::A, 3);
    award(engine, TeamId::B, 3);
    REQUIRE(display(engine).is_golden_point);
    award(engine, TeamId::B);
    REQUIRE(engine.state().current_set.games_b == 1);
}

TEST_CASE("quick set with win-by-two margin policy", "[sets][custom]") {
    MatchConfig config = preset_club_mini_set();
    config.normal_set.win_by_two_games = true;  // custom club variant
    MatchEngine engine = started(config);

    reach_games(engine, 2, 2);
    win_game(engine, TeamId::A);  // 3-2: not enough with win-by-two
    REQUIRE(engine.state().lifecycle == MatchLifecycle::Active);
    REQUIRE(engine.state().current_set.games_a == 3);

    win_game(engine, TeamId::A);  // 4-2: match
    REQUIRE(engine.state().lifecycle == MatchLifecycle::Completed);
    REQUIRE(engine.state().winner == TeamId::A);
}
