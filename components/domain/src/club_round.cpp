#include "padel/domain/club_round.hpp"

namespace padel::domain {
namespace {

constexpr ClubPairing kSet1Pairing{{0, 1}, {2, 3}};

bool on_team(const std::array<std::uint8_t, 2>& team, std::uint8_t slot) {
    return team[0] == slot || team[1] == slot;
}

}  // namespace

ClubRound::ClubRound(std::uint32_t coin_seed, ClubForbiddenPairs forbidden)
    : coin_seed_(coin_seed), forbidden_(forbidden) {}

ClubPairing ClubRound::current_pairing() const {
    if (stage_ == ClubStage::Set1) {
        return kSet1Pairing;
    }
    // Mix: the Set 1 winners split up and each takes a loser. Preferred is the
    // sheet's convention (first winner pairs with the first loser); the
    // swapped split is the only alternative, and it is what keeps a barred
    // pair - a Top 2 from the previous round - off the same side.
    const SetRecord& set1 = sets_[0];
    const auto& winners = set1.winner == TeamId::A ? set1.pairing.team_a : set1.pairing.team_b;
    const auto& losers = set1.winner == TeamId::A ? set1.pairing.team_b : set1.pairing.team_a;

    const ClubPairing preferred{{winners[0], losers[0]}, {winners[1], losers[1]}};
    if (!forbidden_.bars(preferred.team_a[0], preferred.team_a[1]) &&
        !forbidden_.bars(preferred.team_b[0], preferred.team_b[1])) {
        return preferred;
    }
    const ClubPairing swapped{{winners[0], losers[1]}, {winners[1], losers[0]}};
    if (!forbidden_.bars(swapped.team_a[0], swapped.team_a[1]) &&
        !forbidden_.bars(swapped.team_b[0], swapped.team_b[1])) {
        return swapped;
    }
    // Both splits are barred, which means the set 1 picks were illegal in the
    // first place. Fall back rather than deadlock the round.
    return preferred;
}

void ClubRound::record_set_result(TeamId winner, std::uint8_t winner_games,
                                  std::uint8_t loser_games) {
    if (stage_ == ClubStage::Complete) {
        return;
    }
    const std::size_t index = stage_ == ClubStage::Set1 ? 0 : 1;
    sets_[index] = SetRecord{current_pairing(), winner, winner_games, loser_games};
    if (stage_ == ClubStage::Set1) {
        stage_ = ClubStage::Set2;
    } else {
        stage_ = ClubStage::Complete;
        finalize();
    }
}

bool ClubRound::undo_last_set_result() {
    if (stage_ == ClubStage::Set1) {
        return false;
    }
    if (stage_ == ClubStage::Complete) {
        sets_[1] = SetRecord{};
        stage_ = ClubStage::Set2;
        ranked_ = {};
        wins_ = {};
        diff_ = {};
        coin_flipped_ = false;
        coin_winner_ = 0;
        return true;
    }
    sets_[0] = SetRecord{};
    stage_ = ClubStage::Set1;
    return true;
}

int ClubRound::recorded_set_count() const {
    switch (stage_) {
        case ClubStage::Set1:
            return 0;
        case ClubStage::Set2:
            return 1;
        case ClubStage::Complete:
            return 2;
    }
    return 0;
}

ClubSetResult ClubRound::set_result(int index) const {
    if (index < 0 || index >= recorded_set_count()) {
        return {};
    }
    const SetRecord& set = sets_[static_cast<std::size_t>(index)];
    const bool a_won = set.winner == TeamId::A;
    return ClubSetResult{set.pairing, set.winner, a_won ? set.winner_games : set.loser_games,
                         a_won ? set.loser_games : set.winner_games};
}

void ClubRound::finalize() {
    wins_ = {};
    diff_ = {};
    for (const SetRecord& set : sets_) {
        const auto& winners = set.winner == TeamId::A ? set.pairing.team_a : set.pairing.team_b;
        const int margin = static_cast<int>(set.winner_games) - static_cast<int>(set.loser_games);
        for (std::uint8_t slot = 0; slot < 4; ++slot) {
            if (on_team(winners, slot)) {
                ++wins_[slot];
                diff_[slot] += margin;
            } else {
                diff_[slot] -= margin;
            }
        }
    }

    // With mixed pairings there is always exactly one 2-win player, two
    // 1-win players, and one 0-win player. Rank: 2-win first, 0-win last,
    // the 1-win pair ordered by differential (coin flip on a tie).
    std::uint8_t two_wins = 0;
    std::uint8_t zero_wins = 0;
    std::array<std::uint8_t, 2> one_win{};
    std::size_t one_count = 0;
    for (std::uint8_t slot = 0; slot < 4; ++slot) {
        if (wins_[slot] == 2) {
            two_wins = slot;
        } else if (wins_[slot] == 0) {
            zero_wins = slot;
        } else {
            one_win[one_count++] = slot;
        }
    }

    coin_flipped_ = false;
    if (diff_[one_win[0]] < diff_[one_win[1]]) {
        std::swap(one_win[0], one_win[1]);
    } else if (diff_[one_win[0]] == diff_[one_win[1]]) {
        coin_flipped_ = true;
        if ((coin_seed_ & 1) != 0) {
            std::swap(one_win[0], one_win[1]);
        }
        coin_winner_ = one_win[0];
    }

    ranked_ = {two_wins, one_win[0], one_win[1], zero_wins};
}

std::array<ClubStanding, 4> ClubRound::standings() const {
    std::array<ClubStanding, 4> result{};
    for (std::size_t i = 0; i < 4; ++i) {
        const std::uint8_t slot = ranked_[i];
        result[i] = ClubStanding{slot, wins_[slot], diff_[slot]};
    }
    return result;
}

std::array<std::uint8_t, 2> ClubRound::top2() const {
    return {ranked_[0], ranked_[1]};
}

std::array<std::uint8_t, 2> ClubRound::bottom2() const {
    return {ranked_[2], ranked_[3]};
}

}  // namespace padel::domain
