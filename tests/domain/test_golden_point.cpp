#include <catch2/catch_test_macros.hpp>

#include "domain/helpers.hpp"

using namespace padel;
using namespace padel::domain;
using namespace padel::domain::testing;

TEST_CASE("golden point: 40-40 shows golden point, not deuce", "[golden]") {
    MatchEngine engine = started(preset_standard_golden_point());
    award(engine, TeamId::A, 3);
    award(engine, TeamId::B, 3);

    const DisplayState d = display(engine);
    REQUIRE(d.is_golden_point);
    REQUIRE_FALSE(d.is_deuce);
    REQUIRE(d.points_a == "40");
    REQUIRE(d.points_b == "40");
}

TEST_CASE("golden point: next Team A point wins the game", "[golden]") {
    MatchEngine engine = started(preset_standard_golden_point());
    award(engine, TeamId::A, 3);
    award(engine, TeamId::B, 3);
    award(engine, TeamId::A);

    const DisplayState d = display(engine);
    REQUIRE(d.games_a == 1);
    REQUIRE(d.games_b == 0);
    REQUIRE(d.points_a == "0");
    REQUIRE_FALSE(d.is_golden_point);
}

TEST_CASE("golden point: next Team B point wins the game", "[golden]") {
    MatchEngine engine = started(preset_standard_golden_point());
    award(engine, TeamId::A, 3);
    award(engine, TeamId::B, 3);
    award(engine, TeamId::B);

    const DisplayState d = display(engine);
    REQUIRE(d.games_a == 0);
    REQUIRE(d.games_b == 1);
}

TEST_CASE("golden point: normal wins before 40-40 unchanged", "[golden]") {
    MatchEngine engine = started(preset_standard_golden_point());
    award(engine, TeamId::A, 3);
    award(engine, TeamId::B, 2);
    award(engine, TeamId::A);
    REQUIRE(display(engine).games_a == 1);
}
