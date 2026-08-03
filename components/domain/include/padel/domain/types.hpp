#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "padel/common/ids.hpp"

namespace padel::domain {

enum class GameRule : std::uint8_t {
    Advantage,
    GoldenPoint,
};

enum class FinalSetRule : std::uint8_t {
    FullSet,
    MatchTiebreak,
};

struct SetRule {
    std::uint8_t games_to_win = 6;
    bool win_by_two_games = true;
    bool tiebreak_enabled = true;
    std::uint8_t tiebreak_at_games = 6;
    std::uint8_t tiebreak_points_to_win = 7;
    bool tiebreak_win_by_two = true;
};

struct MatchConfig {
    GameRule game_rule = GameRule::Advantage;
    SetRule normal_set{};
    std::uint8_t sets_to_win = 2;
    FinalSetRule final_set_rule = FinalSetRule::FullSet;
    std::uint8_t match_tiebreak_points_to_win = 10;
    bool match_tiebreak_win_by_two = true;
    bool track_serving_team = true;
};

enum class MatchLifecycle : std::uint8_t {
    NotStarted,
    Active,
    Paused,
    Completed,
};

struct GameState {
    std::uint8_t raw_points_a = 0;
    std::uint8_t raw_points_b = 0;
};

struct SetScore {
    std::uint8_t games_a = 0;
    std::uint8_t games_b = 0;
    std::optional<std::uint8_t> tiebreak_points_a{};
    std::optional<std::uint8_t> tiebreak_points_b{};
};

// Best-of-N padel matches are bounded; fixed capacity avoids heap growth in
// firmware hot paths (spec section 8.4).
inline constexpr std::size_t kMaxSets = 5;

struct MatchState {
    MatchId match_id{};
    MatchConfig config{};
    MatchLifecycle lifecycle{MatchLifecycle::NotStarted};
    TeamId serving_team{TeamId::A};
    GameState current_game{};
    std::array<SetScore, kMaxSets> completed_sets{};
    std::uint8_t completed_set_count = 0;
    SetScore current_set{};
    bool in_tiebreak = false;
    bool in_match_tiebreak = false;
    std::uint8_t tiebreak_points_a = 0;
    std::uint8_t tiebreak_points_b = 0;
    std::uint8_t sets_won_a = 0;
    std::uint8_t sets_won_b = 0;
    std::uint64_t revision = 0;
    std::optional<TeamId> winner{};
};

// --- Scoring presets -------------------------------------------------------

// Standard padel: advantage games, best of three, sets to 6, tiebreak at 6-6.
MatchConfig preset_standard_advantage();

// Standard sets but golden point at 40-40 (WPT rule).
MatchConfig preset_standard_golden_point();

// Club mini-set (ADR-0002): single set, first to 3 games, no win-by-two,
// no tiebreak, golden point. One "match" is one mini-set of the club round.
MatchConfig preset_club_mini_set();

// Best of three with advantage games; deciding set is a match tiebreak to 10.
MatchConfig preset_match_tiebreak_final();

}  // namespace padel::domain
