// Club round logic (one court, four players) per the club rotation sheets:
//
//   Set 1: mini-set first to 3 (Team A pair vs Team B pair)
//   Set 2: mix within the court — the Set 1 winners split up, each takes
//          a loser
//   Top 2 = the 2-win player + the 1-win player with the better game
//          differential (a 3-0 win beats a 3-2 win); a tied differential is
//          resolved by an automatic coin flip (announced, never a button)
//
// Pure logic over player *slots* 0..3 (0,1 = Team A picks, 2,3 = Team B
// picks); names live in the application layer. The multi-court rotation
// that consumes Top2/Bottom2 is out of scope (M8) — this class is the
// per-court piece.
#pragma once

#include <array>
#include <cstdint>

#include "padel/common/ids.hpp"

namespace padel::domain {

// Slots on each side of one mini-set.
struct ClubPairing {
    std::array<std::uint8_t, 2> team_a{};
    std::array<std::uint8_t, 2> team_b{};
};

enum class ClubStage : std::uint8_t {
    Set1,      // waiting for the first mini-set result
    Set2,      // set 1 recorded, waiting for the mixed set
    Complete,  // standings available
};

struct ClubStanding {
    std::uint8_t slot = 0;
    std::uint8_t wins = 0;
    int differential = 0;  // +games won side, -games lost side, both sets
};

class ClubRound {
public:
    // coin_seed feeds the automatic coin flip on a tied differential;
    // hosts pass a random word, tests pass a known one (flip = seed & 1).
    explicit ClubRound(std::uint32_t coin_seed = 0);

    ClubStage stage() const { return stage_; }

    // Pairing for the set the round is currently waiting on.
    // Set 1: {0,1} vs {2,3}. Set 2: winners split, each takes a loser
    // (with this slot layout that is always {0,2} vs {1,3}).
    ClubPairing current_pairing() const;

    // Records the result of the current set (first to 3: winner_games is 3).
    // Ignored when the round is already complete.
    void record_set_result(TeamId winner, std::uint8_t winner_games,
                           std::uint8_t loser_games);

    // --- Available once stage() == Complete --------------------------------
    // Sorted best -> worst (wins desc, then differential desc; the coin
    // flip decides the middle pair on a tie).
    std::array<ClubStanding, 4> standings() const;
    std::array<std::uint8_t, 2> top2() const;
    std::array<std::uint8_t, 2> bottom2() const;

    // True when the middle two (the 1-win players) tied on differential and
    // the coin decided; coin_flip_winner() is the slot that took the last
    // Top 2 spot.
    bool decided_by_coin_flip() const { return coin_flipped_; }
    std::uint8_t coin_flip_winner() const { return coin_winner_; }

    // The pair that must not play together in the next round (this round's
    // Top 2, per the sheet's always-winning-team rule).
    std::array<std::uint8_t, 2> forbidden_pair() const { return top2(); }

private:
    struct SetRecord {
        ClubPairing pairing{};
        TeamId winner{TeamId::A};
        std::uint8_t winner_games = 0;
        std::uint8_t loser_games = 0;
    };

    void finalize();

    std::uint32_t coin_seed_;
    ClubStage stage_ = ClubStage::Set1;
    std::array<SetRecord, 2> sets_{};

    std::array<std::uint8_t, 4> ranked_{};  // slots, best -> worst
    std::array<std::uint8_t, 4> wins_{};
    std::array<int, 4> diff_{};
    bool coin_flipped_ = false;
    std::uint8_t coin_winner_ = 0;
};

}  // namespace padel::domain
