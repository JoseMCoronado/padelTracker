#include <catch2/catch_test_macros.hpp>

#include "domain/helpers.hpp"

using namespace padel;
using namespace padel::domain;
using namespace padel::domain::testing;

TEST_CASE("advantage game: 0 -> 15 -> 30 -> 40 -> game", "[advantage]") {
    MatchEngine engine = started(preset_standard_advantage());

    const char* expected[] = {"0", "15", "30", "40"};
    for (int i = 0; i < 4; ++i) {
        REQUIRE(display(engine).points_a == expected[i]);
        REQUIRE(display(engine).points_b == "0");
        award(engine, TeamId::A);
    }
    // Fourth point wins the game.
    REQUIRE(display(engine).points_a == "0");
    REQUIRE(display(engine).games_a == 1);
    REQUIRE(display(engine).games_b == 0);
}

TEST_CASE("advantage game: 40-0 game (love game)", "[advantage]") {
    MatchEngine engine = started(preset_standard_advantage());
    award(engine, TeamId::B, 4);
    REQUIRE(display(engine).games_b == 1);
    REQUIRE(display(engine).points_a == "0");
    REQUIRE(display(engine).points_b == "0");
}

TEST_CASE("advantage game: 40-30 then game", "[advantage]") {
    MatchEngine engine = started(preset_standard_advantage());
    award(engine, TeamId::A, 3);
    award(engine, TeamId::B, 2);
    REQUIRE(display(engine).points_a == "40");
    REQUIRE(display(engine).points_b == "30");
    award(engine, TeamId::A);
    REQUIRE(display(engine).games_a == 1);
}

TEST_CASE("advantage game: deuce -> AD A -> deuce -> AD B -> game B", "[advantage]") {
    MatchEngine engine = started(preset_standard_advantage());
    award(engine, TeamId::A, 3);
    award(engine, TeamId::B, 3);

    DisplayState d = display(engine);
    REQUIRE(d.is_deuce);
    REQUIRE(d.points_a == "40");
    REQUIRE(d.points_b == "40");

    award(engine, TeamId::A);
    d = display(engine);
    REQUIRE(d.points_a == "AD");
    REQUIRE(d.points_b == "40");
    REQUIRE_FALSE(d.is_deuce);

    award(engine, TeamId::B);
    REQUIRE(display(engine).is_deuce);

    award(engine, TeamId::B);
    d = display(engine);
    REQUIRE(d.points_a == "40");
    REQUIRE(d.points_b == "AD");

    award(engine, TeamId::B);
    d = display(engine);
    REQUIRE(d.games_b == 1);
    REQUIRE(d.points_a == "0");
    REQUIRE(d.points_b == "0");
}

TEST_CASE("advantage game: many repeated deuce cycles", "[advantage]") {
    MatchEngine engine = started(preset_standard_advantage());
    award(engine, TeamId::A, 3);
    award(engine, TeamId::B, 3);

    for (int cycle = 0; cycle < 20; ++cycle) {
        award(engine, TeamId::A);  // AD A
        REQUIRE(display(engine).points_a == "AD");
        award(engine, TeamId::B);  // back to deuce
        REQUIRE(display(engine).is_deuce);
    }
    // Still no game won.
    REQUIRE(display(engine).games_a == 0);
    REQUIRE(display(engine).games_b == 0);

    award(engine, TeamId::B);
    award(engine, TeamId::B);
    REQUIRE(display(engine).games_b == 1);
}

TEST_CASE("display projection covers all pre-deuce states", "[advantage][projection]") {
    const char* labels[] = {"0", "15", "30", "40"};
    for (int a = 0; a <= 3; ++a) {
        for (int b = 0; b <= 3; ++b) {
            if (a == 3 && b == 3) {
                continue;  // deuce case covered elsewhere
            }
            MatchEngine engine = started(preset_standard_advantage());
            award(engine, TeamId::A, static_cast<std::size_t>(a));
            award(engine, TeamId::B, static_cast<std::size_t>(b));
            const DisplayState d = display(engine);
            REQUIRE(d.points_a == labels[a]);
            REQUIRE(d.points_b == labels[b]);
            REQUIRE_FALSE(d.is_deuce);
            REQUIRE_FALSE(d.is_golden_point);
            REQUIRE_FALSE(d.is_tiebreak);
        }
    }
}

TEST_CASE("serving team alternates at game boundaries", "[advantage][serving]") {
    MatchEngine engine = started(preset_standard_advantage(), TeamId::A);
    REQUIRE(engine.state().serving_team == TeamId::A);
    win_game(engine, TeamId::A);
    REQUIRE(engine.state().serving_team == TeamId::B);
    win_game(engine, TeamId::B);
    REQUIRE(engine.state().serving_team == TeamId::A);
}

TEST_CASE("organizer can set serving team explicitly", "[serving]") {
    MatchEngine engine = started(preset_standard_advantage(), TeamId::A);
    REQUIRE(engine.handle(SetServingTeam{TeamId::B}).has_value());
    REQUIRE(engine.state().serving_team == TeamId::B);
}
