#include "padel/domain/projection.hpp"

#include <algorithm>
#include <unordered_set>

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

        SetLine line{};
        line.games_a = set.games_a;
        line.games_b = set.games_b;
        line.tiebreak_points_a = set.tiebreak_points_a;
        line.tiebreak_points_b = set.tiebreak_points_b;
        line.completed = true;
        line.winner = set.games_a > set.games_b ? TeamId::A : TeamId::B;
        display.sets.push_back(line);
    }
    display.game_differential = differential;

    // The set in progress is the last scoreboard column. A finished match has
    // none: its last set already moved into completed_sets.
    if (state.lifecycle != MatchLifecycle::NotStarted &&
        state.lifecycle != MatchLifecycle::Completed) {
        SetLine line{};
        line.games_a = state.current_set.games_a;
        line.games_b = state.current_set.games_b;
        if (state.in_tiebreak) {
            line.tiebreak_points_a = state.tiebreak_points_a;
            line.tiebreak_points_b = state.tiebreak_points_b;
        }
        display.sets.push_back(line);
    }

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

MatchStats summarize(const std::vector<StoredEvent>& journal) {
    std::unordered_set<EventId> compensated;
    for (const StoredEvent& stored : journal) {
        if (const auto* undo = std::get_if<ScoringActionUndone>(&stored.payload)) {
            compensated.insert(undo->undone_event_id);
        }
    }

    MatchStats stats{};
    std::optional<TeamId> streak_team{};
    std::uint32_t streak = 0;
    for (const StoredEvent& stored : journal) {
        if (std::holds_alternative<MatchCreated>(stored.payload) ||
            std::holds_alternative<MatchReset>(stored.payload)) {
            stats = MatchStats{};  // a reset starts a new match
            streak_team.reset();
            streak = 0;
            continue;
        }
        const auto* point = std::get_if<PointAwarded>(&stored.payload);
        if (point == nullptr || compensated.count(stored.id) != 0) {
            continue;
        }
        if (point->team == TeamId::A) {
            ++stats.points_a;
        } else {
            ++stats.points_b;
        }
        streak = streak_team && *streak_team == point->team ? streak + 1 : 1;
        streak_team = point->team;
        if (streak > stats.longest_streak) {
            stats.longest_streak = streak;
            stats.longest_streak_team = point->team;
        }
    }
    return stats;
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
