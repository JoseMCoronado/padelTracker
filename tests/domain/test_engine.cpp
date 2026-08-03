#include <catch2/catch_test_macros.hpp>

#include "domain/helpers.hpp"

using namespace padel;
using namespace padel::domain;
using namespace padel::domain::testing;

TEST_CASE("engine: revision is monotonic across undo", "[engine]") {
    MatchEngine engine = started(preset_standard_advantage());
    award(engine, TeamId::A, 2);
    const std::uint64_t before = engine.state().revision;

    engine.handle(UndoLastScoringAction{});
    REQUIRE(engine.state().revision == before + 1);
}

TEST_CASE("engine: every accepted command appends exactly one event", "[engine]") {
    MatchEngine engine(preset_standard_advantage());
    REQUIRE(engine.journal().size() == 1);  // MatchCreated

    engine.handle(StartMatch{TeamId::A});
    REQUIRE(engine.journal().size() == 2);

    engine.handle(AwardPoint{TeamId::A, InputSource::Simulator});
    REQUIRE(engine.journal().size() == 3);

    // Rejected commands append nothing.
    engine.handle(StartMatch{TeamId::A});
    REQUIRE(engine.journal().size() == 3);
}

TEST_CASE("engine: replay of a recovered journal reproduces state", "[engine][replay]") {
    MatchEngine original = started(preset_standard_golden_point());
    award(original, TeamId::A, 4);  // game A
    award(original, TeamId::B, 2);
    original.handle(UndoLastScoringAction{});
    original.handle(PauseMatch{});

    // Simulate boot recovery: rebuild an engine from the journaled events.
    MatchEngine recovered =
        MatchEngine::replay(original.journal(), preset_standard_golden_point());

    REQUIRE(recovered.state().lifecycle == original.state().lifecycle);
    REQUIRE(recovered.state().current_game.raw_points_a ==
            original.state().current_game.raw_points_a);
    REQUIRE(recovered.state().current_game.raw_points_b ==
            original.state().current_game.raw_points_b);
    REQUIRE(recovered.state().current_set.games_a == original.state().current_set.games_a);
    REQUIRE(recovered.state().revision == original.state().revision);

    // The recovered engine continues accepting commands with fresh event ids.
    recovered.handle(ResumeMatch{});
    REQUIRE(recovered.handle(AwardPoint{TeamId::B, InputSource::Simulator}).has_value());
    REQUIRE(recovered.journal().back().id > original.journal().back().id);
}

TEST_CASE("engine: replay of an empty journal starts a fresh match", "[engine][replay]") {
    MatchEngine engine = MatchEngine::replay({}, preset_club_mini_set());
    REQUIRE(engine.state().lifecycle == MatchLifecycle::NotStarted);
    REQUIRE(engine.state().config.sets_to_win == 1);
}

TEST_CASE("engine: single-set custom config with match-tiebreak final starts in tiebreak",
          "[engine][custom]") {
    MatchConfig config{};
    config.sets_to_win = 1;
    config.final_set_rule = FinalSetRule::MatchTiebreak;
    MatchEngine engine = started(config);

    REQUIRE(engine.state().in_match_tiebreak);
    award(engine, TeamId::A, 10);
    REQUIRE(engine.state().lifecycle == MatchLifecycle::Completed);
    REQUIRE(engine.state().winner == TeamId::A);
}
