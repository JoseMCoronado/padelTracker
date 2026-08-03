#include <catch2/catch_test_macros.hpp>

#include "domain/helpers.hpp"

using namespace padel;
using namespace padel::domain;
using namespace padel::domain::testing;

TEST_CASE("match: straight-sets win (best of three)", "[match]") {
    MatchEngine engine = started(preset_standard_advantage());
    win_games(engine, TeamId::A, 6);
    REQUIRE(engine.state().lifecycle == MatchLifecycle::Active);
    win_games(engine, TeamId::A, 6);

    const MatchState& s = engine.state();
    REQUIRE(s.lifecycle == MatchLifecycle::Completed);
    REQUIRE(s.winner == TeamId::A);
    REQUIRE(s.sets_won_a == 2);
    REQUIRE(s.sets_won_b == 0);
}

TEST_CASE("match: three-set match with full final set", "[match]") {
    MatchEngine engine = started(preset_standard_advantage());
    win_games(engine, TeamId::A, 6);  // 1-0
    win_games(engine, TeamId::B, 6);  // 1-1
    REQUIRE(engine.state().lifecycle == MatchLifecycle::Active);
    REQUIRE_FALSE(engine.state().in_tiebreak);  // FullSet rule: normal set

    reach_games(engine, 7, 5);  // final set 7-5
    const MatchState& s = engine.state();
    REQUIRE(s.lifecycle == MatchLifecycle::Completed);
    REQUIRE(s.winner == TeamId::A);
    REQUIRE(s.completed_set_count == 3);
    REQUIRE(display(engine).set_history == std::vector<std::string>{"6-0", "0-6", "7-5"});
}

TEST_CASE("match: deciding set as match tiebreak to 10", "[match][tiebreak]") {
    MatchEngine engine = started(preset_match_tiebreak_final());
    win_games(engine, TeamId::A, 6);  // 1-0
    win_games(engine, TeamId::B, 6);  // 1-1 -> match tiebreak

    const MatchState& mid = engine.state();
    REQUIRE(mid.in_tiebreak);
    REQUIRE(mid.in_match_tiebreak);
    REQUIRE(display(engine).is_match_tiebreak);

    award(engine, TeamId::B, 9);
    REQUIRE(engine.state().lifecycle == MatchLifecycle::Active);  // 9-0 not done
    award(engine, TeamId::B);  // 10-0

    const MatchState& s = engine.state();
    REQUIRE(s.lifecycle == MatchLifecycle::Completed);
    REQUIRE(s.winner == TeamId::B);
    REQUIRE(s.completed_sets[2].games_b == 1);
    REQUIRE(s.completed_sets[2].tiebreak_points_b.value() == 10);
}

TEST_CASE("match: match tiebreak requires win by two", "[match][tiebreak]") {
    MatchEngine engine = started(preset_match_tiebreak_final());
    win_games(engine, TeamId::A, 6);
    win_games(engine, TeamId::B, 6);

    for (int i = 0; i < 9; ++i) {
        award(engine, TeamId::A);
        award(engine, TeamId::B);
    }
    // 9-9 -> 10-9 is not enough.
    award(engine, TeamId::A);
    REQUIRE(engine.state().lifecycle == MatchLifecycle::Active);
    award(engine, TeamId::A);  // 11-9
    REQUIRE(engine.state().lifecycle == MatchLifecycle::Completed);
    REQUIRE(engine.state().winner == TeamId::A);
}

TEST_CASE("match: no points accepted after completion", "[match][lifecycle]") {
    MatchEngine engine = started(preset_club_mini_set());
    win_games(engine, TeamId::A, 3);
    REQUIRE(engine.state().lifecycle == MatchLifecycle::Completed);

    const auto result = engine.handle(AwardPoint{TeamId::B, InputSource::Simulator});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == CommandError::MatchCompleted);
    REQUIRE(engine.state().winner == TeamId::A);
}

TEST_CASE("match: pause rejects points, resume accepts again", "[match][lifecycle]") {
    MatchEngine engine = started(preset_standard_advantage());
    REQUIRE(engine.handle(PauseMatch{}).has_value());

    const auto rejected = engine.handle(AwardPoint{TeamId::A, InputSource::Simulator});
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error() == CommandError::MatchPausedError);

    REQUIRE(engine.handle(ResumeMatch{}).has_value());
    REQUIRE(engine.handle(AwardPoint{TeamId::A, InputSource::Simulator}).has_value());
    REQUIRE(engine.state().current_game.raw_points_a == 1);
}

TEST_CASE("match: manual finish completes the match", "[match][lifecycle]") {
    MatchEngine engine = started(preset_standard_advantage());
    award(engine, TeamId::A, 2);
    REQUIRE(engine.handle(FinishMatchManually{TeamId::B}).has_value());
    REQUIRE(engine.state().lifecycle == MatchLifecycle::Completed);
    REQUIRE(engine.state().winner == TeamId::B);
}

TEST_CASE("match: points before start are rejected", "[match][lifecycle]") {
    MatchEngine engine(preset_standard_advantage());
    const auto result = engine.handle(AwardPoint{TeamId::A, InputSource::Simulator});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == CommandError::MatchNotStarted);
}

TEST_CASE("match: reset returns to a fresh not-started state", "[match][lifecycle]") {
    MatchEngine engine = started(preset_standard_advantage());
    win_games(engine, TeamId::A, 3);
    REQUIRE(engine.handle(ResetMatch{}).has_value());

    const MatchState& s = engine.state();
    REQUIRE(s.lifecycle == MatchLifecycle::NotStarted);
    REQUIRE(s.current_set.games_a == 0);
    REQUIRE(s.sets_won_a == 0);
    REQUIRE_FALSE(s.winner.has_value());
    // Journal history is preserved (events are never deleted).
    REQUIRE(engine.journal().size() > 1);
}
