// Court-side orchestration of one club round (see domain/club_round.hpp):
// maps roster players onto round slots, derives team labels for each
// mini-set, consumes completed MatchStates, and writes the per-player
// results log when the round finishes. Hosts (court-sim / firmware) own the
// CourtService match lifecycle; this class only tells them who plays whom.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "padel/application/clock.hpp"
#include "padel/application/roster.hpp"
#include "padel/domain/club_round.hpp"
#include "padel/domain/types.hpp"

namespace padel::application {

class ClubController {
public:
    enum class StartError {
        ForbiddenPair,  // a barred pair (previous Top 2 or a crown) as teammates
        RoundActive,
        DuplicatePlayer,
    };

    struct TeamLabels {
        std::string team_a;  // "JOSE & ZOE"
        std::string team_b;
    };

    // A finished mini-set with the names that played it; the mix swaps
    // partners, so set 1 keeps its own pairing on the scoreboard.
    struct SetScoreline {
        std::string team_a;  // "JOSE & RUXANDRA"
        std::string team_b;
        std::uint8_t games_a = 0;
        std::uint8_t games_b = 0;
        TeamId winner{TeamId::A};
    };

    struct StandingRow {
        Player player;
        std::uint8_t wins = 0;
        int differential = 0;
        bool top2 = false;
    };

    // Two players who must never be teammates this round, by player id.
    using ForbiddenPair = std::array<std::uint32_t, 2>;

    ClubController(IResultsLog& results, const IClock& clock);

    // players = {teamA[0], teamA[1], teamB[0], teamB[1]} from the picker.
    // coin_seed feeds the automatic tie-break flip; hosts pass randomness.
    // crowned carries the pairs the organizer marked in the picker (the Top 2
    // that came up from another court); the previous round's own Top 2 is
    // added automatically. Both are kept apart in set 1 and in the mix.
    std::optional<StartError> start_round(const std::array<Player, 4>& players,
                                          std::uint32_t coin_seed,
                                          const std::vector<ForbiddenPair>& crowned = {});

    bool round_active() const { return round_.has_value(); }
    domain::ClubStage stage() const;
    int set_number() const;  // 1 or 2 while active

    // Teams for the mini-set the round is waiting on.
    TeamLabels current_set_teams() const;

    // "JOSE & ZOE took set 1 (3-1)" once set 1 is in; shown on the mix screen.
    const std::string& last_set_summary() const { return last_set_summary_; }

    // The mini-sets already played, oldest first, each with its own pairing.
    std::vector<SetScoreline> recorded_sets() const;

    // Feed the completed mini-set. On the second set this finalizes the
    // standings; the results log is written by finish_round().
    void on_set_complete(const domain::MatchState& state);

    // Rewinds the most recently recorded mini-set, so an undo that reopens a
    // finished match on the court display also reopens the round.
    bool undo_last_set();

    // Available once stage() == Complete.
    std::vector<StandingRow> standings() const;
    // "COIN FLIP: JOSE takes the last TOP 2 spot", or "" when not flipped.
    std::string coin_flip_announcement() const;

    // Ends the round; the Top 2 becomes the forbidden pair for the next one.
    void finish_round();

    const std::optional<std::array<std::uint32_t, 2>>& forbidden_pair_ids() const {
        return forbidden_ids_;
    }

private:
    std::string label_for(const std::array<std::uint8_t, 2>& slots) const;
    void write_results_log();
    // Previous round's Top 2 plus the organizer's crowns, deduplicated and
    // capped at what ClubRound can carry.
    std::vector<ForbiddenPair> collect_forbidden(const std::vector<ForbiddenPair>& crowned) const;

    IResultsLog& results_;
    const IClock& clock_;

    std::optional<domain::ClubRound> round_{};
    std::array<Player, 4> players_{};
    std::optional<std::array<std::uint32_t, 2>> forbidden_ids_{};
    // Index 0 = set 1, index 1 = set 2; last_set_summary() reads the newest.
    std::array<std::string, 2> set_summaries_{};
    std::string last_set_summary_;
};

}  // namespace padel::application
