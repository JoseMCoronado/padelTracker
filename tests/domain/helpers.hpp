#pragma once

#include <cstddef>

#include "padel/domain/match_engine.hpp"
#include "padel/domain/projection.hpp"

namespace padel::domain::testing {

inline MatchEngine started(MatchConfig config, TeamId serving = TeamId::A) {
    MatchEngine engine(config);
    engine.handle(StartMatch{serving});
    return engine;
}

inline void award(MatchEngine& engine, TeamId team, std::size_t count = 1) {
    for (std::size_t i = 0; i < count; ++i) {
        engine.handle(AwardPoint{team, InputSource::Simulator});
    }
}

// Wins one advantage/golden game for `team` from 0-0 (4 straight points).
inline void win_game(MatchEngine& engine, TeamId team) {
    award(engine, team, 4);
}

// Wins `count` games in a row for `team` (each from 0-0).
inline void win_games(MatchEngine& engine, TeamId team, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        win_game(engine, team);
    }
}

// Drives the current set to the given game score, alternating so no set
// completes early. Assumes the current set is at 0-0 and target is a valid
// in-progress or just-completed set score.
inline void reach_games(MatchEngine& engine, std::uint8_t games_a, std::uint8_t games_b) {
    // Interleave: give B one game per A game while both have games remaining,
    // then finish the leftovers. Keeps intermediate scores legal (never lets
    // one team finish the set before the target).
    std::uint8_t a = 0;
    std::uint8_t b = 0;
    while (a < games_a || b < games_b) {
        if (a < games_a && (a <= b || b >= games_b)) {
            win_game(engine, TeamId::A);
            ++a;
        } else {
            win_game(engine, TeamId::B);
            ++b;
        }
    }
}

inline DisplayState display(const MatchEngine& engine) {
    return project(engine.state());
}

}  // namespace padel::domain::testing
