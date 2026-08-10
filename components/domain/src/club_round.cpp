#include "padel/domain/club_round.hpp"

namespace padel::domain {
namespace {

constexpr ClubPairing kSet1Pairing{{0, 1}, {2, 3}};

bool on_team(const std::array<std::uint8_t, 2>& team, std::uint8_t slot) {
    return team[0] == slot || team[1] == slot;
}

}  // namespace

ClubRound::ClubRound(std::uint32_t coin_seed) : coin_seed_(coin_seed) {}

ClubPairing ClubRound::current_pairing() const {
    if (stage_ == ClubStage::Set1) {
        return kSet1Pairing;
    }
    // Mix: the Set 1 winners split up and each takes a loser. Preserve the
    // sheet's convention (first winner pairs with the first loser).
    const SetRecord& set1 = sets_[0];
    const auto& winners = set1.winner == TeamId::A ? set1.pairing.team_a : set1.pairing.team_b;
    const auto& losers = set1.winner == TeamId::A ? set1.pairing.team_b : set1.pairing.team_a;
    return ClubPairing{{winners[0], losers[0]}, {winners[1], losers[1]}};
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
