#include "padel/domain/projection.hpp"

#include <algorithm>

namespace padel::domain {
namespace {

const char* point_label(std::uint8_t raw) {
    switch (raw) {
        case 0:
            return "0";
        case 1:
            return "15";
        case 2:
            return "30";
        default:
            return "40";
    }
}

std::string set_history_entry(const SetScore& set) {
    std::string entry = std::to_string(set.games_a) + "-" + std::to_string(set.games_b);
    if (set.tiebreak_points_a && set.tiebreak_points_b) {
        const std::uint8_t loser_points = std::min(*set.tiebreak_points_a, *set.tiebreak_points_b);
        entry += "(" + std::to_string(loser_points) + ")";
    }
    return entry;
}

}  // namespace

DisplayState project(const MatchState& state) {
    DisplayState display{};
    display.lifecycle = state.lifecycle;
    display.serving_team = state.serving_team;
    display.winner = state.winner;

    display.games_a = state.current_set.games_a;
    display.games_b = state.current_set.games_b;
    display.sets_a = state.sets_won_a;
    display.sets_b = state.sets_won_b;

    int differential = static_cast<int>(state.current_set.games_a) -
                       static_cast<int>(state.current_set.games_b);
    for (std::uint8_t i = 0; i < state.completed_set_count; ++i) {
        const SetScore& set = state.completed_sets[i];
        display.set_history.push_back(set_history_entry(set));
        differential += static_cast<int>(set.games_a) - static_cast<int>(set.games_b);
    }
    display.game_differential = differential;

    if (state.in_tiebreak) {
        display.is_tiebreak = true;
        display.is_match_tiebreak = state.in_match_tiebreak;
        display.points_a = std::to_string(state.tiebreak_points_a);
        display.points_b = std::to_string(state.tiebreak_points_b);
        return display;
    }

    const std::uint8_t a = state.current_game.raw_points_a;
    const std::uint8_t b = state.current_game.raw_points_b;

    if (a >= 3 && b >= 3) {
        if (a == b) {
            display.points_a = "40";
            display.points_b = "40";
            if (state.config.game_rule == GameRule::GoldenPoint) {
                display.is_golden_point = true;
            } else {
                display.is_deuce = true;
            }
        } else if (a > b) {
            display.points_a = "AD";
            display.points_b = "40";
        } else {
            display.points_a = "40";
            display.points_b = "AD";
        }
    } else {
        display.points_a = point_label(a);
        display.points_b = point_label(b);
    }
    return display;
}

std::uint8_t display_code(const MatchState& state, TeamId team) {
    if (state.lifecycle != MatchLifecycle::Active) {
        return 255;
    }
    if (state.in_tiebreak) {
        const std::uint8_t points =
            team == TeamId::A ? state.tiebreak_points_a : state.tiebreak_points_b;
        return static_cast<std::uint8_t>(100 + std::min<int>(points, 55));
    }
    const std::uint8_t own =
        team == TeamId::A ? state.current_game.raw_points_a : state.current_game.raw_points_b;
    const std::uint8_t other =
        team == TeamId::A ? state.current_game.raw_points_b : state.current_game.raw_points_a;
    if (own >= 3 && other >= 3) {
        if (own == other) {
            return state.config.game_rule == GameRule::GoldenPoint ? 41 : 3;
        }
        return own > other ? 40 : 3;
    }
    return std::min<std::uint8_t>(own, 3);
}

}  // namespace padel::domain
