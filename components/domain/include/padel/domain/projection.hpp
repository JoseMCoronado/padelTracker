#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "padel/common/ids.hpp"
#include "padel/domain/events.hpp"
#include "padel/domain/types.hpp"

namespace padel::domain {

// One column of the broadcast-style scoreboard: a completed set, or the set
// currently being played.
struct SetLine {
    std::uint8_t games_a = 0;
    std::uint8_t games_b = 0;
    std::optional<std::uint8_t> tiebreak_points_a{};
    std::optional<std::uint8_t> tiebreak_points_b{};
    bool completed = false;
    std::optional<TeamId> winner{};  // set only when completed
};

// View model derived from authoritative MatchState (spec invariant 3.2.7:
// UI state is projected, never maintained independently). Uses std::string
// for host-side display and tests; the LVGL layer consumes the same shape.
struct DisplayState {
    MatchLifecycle lifecycle{MatchLifecycle::NotStarted};

    // Point display: "0"/"15"/"30"/"40"/"AD", or tiebreak points as digits.
    std::string points_a;
    std::string points_b;
    bool is_deuce = false;
    bool is_golden_point = false;  // 40-40 under the golden-point rule
    bool is_tiebreak = false;
    bool is_match_tiebreak = false;

    std::uint8_t games_a = 0;
    std::uint8_t games_b = 0;
    std::uint8_t sets_a = 0;
    std::uint8_t sets_b = 0;

    // Completed sets as "6-4" or "7-6(5)" (loser's tiebreak points).
    std::vector<std::string> set_history;

    // The same sets in structured form, completed ones first and the set in
    // progress last, so a scoreboard can lay them out in columns. Empty
    // before the match starts.
    std::vector<SetLine> sets;

    // Games won by A minus games won by B across the whole match, including
    // the current set. Club Top2/Bottom2 is decided by this (ADR-0002).
    int game_differential = 0;

    TeamId serving_team{TeamId::A};
    std::optional<TeamId> winner{};
};

DisplayState project(const MatchState& state);

// Rally-level totals for the post-match summary. Derived from the journal
// because MatchState only carries the current score; undone points are
// skipped, and everything before the last reset is ignored.
struct MatchStats {
    std::uint32_t points_a = 0;
    std::uint32_t points_b = 0;
    std::uint32_t longest_streak = 0;
    std::optional<TeamId> longest_streak_team{};
};

MatchStats summarize(const std::vector<StoredEvent>& journal);

// Compact one-byte score projection carried in ACK packets for diagnostics
// (docs/RADIO_PROTOCOL.md "Display codes").
std::uint8_t display_code(const MatchState& state, TeamId team);

}  // namespace padel::domain
