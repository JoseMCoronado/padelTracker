#include "padel/application/club_controller.hpp"

#include <algorithm>
#include <cctype>

#include "padel/common/log.hpp"

namespace padel::application {
namespace {

std::string uppercased(const std::string& text) {
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

}  // namespace

ClubController::ClubController(IResultsLog& results, const IClock& clock)
    : results_(results), clock_(clock) {}

std::optional<ClubController::StartError> ClubController::start_round(
    const std::array<Player, 4>& players, std::uint32_t coin_seed) {
    if (round_.has_value()) {
        return StartError::RoundActive;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = i + 1; j < 4; ++j) {
            if (players[i].id == players[j].id) {
                return StartError::DuplicatePlayer;
            }
        }
    }
    if (forbidden_ids_) {
        const auto is_forbidden_pair = [&](const Player& x, const Player& y) {
            return (x.id == (*forbidden_ids_)[0] && y.id == (*forbidden_ids_)[1]) ||
                   (x.id == (*forbidden_ids_)[1] && y.id == (*forbidden_ids_)[0]);
        };
        if (is_forbidden_pair(players[0], players[1]) ||
            is_forbidden_pair(players[2], players[3])) {
            return StartError::ForbiddenPair;
        }
    }

    players_ = players;
    last_set_summary_.clear();
    round_.emplace(coin_seed);
    logging::emit(logging::Level::Info, "club.round_started", "a=%s+%s b=%s+%s",
                  players[0].name.c_str(), players[1].name.c_str(), players[2].name.c_str(),
                  players[3].name.c_str());
    return std::nullopt;
}

domain::ClubStage ClubController::stage() const {
    return round_ ? round_->stage() : domain::ClubStage::Set1;
}

int ClubController::set_number() const {
    return round_ && round_->stage() == domain::ClubStage::Set2 ? 2 : 1;
}

std::string ClubController::label_for(const std::array<std::uint8_t, 2>& slots) const {
    return uppercased(players_[slots[0]].name) + " & " + uppercased(players_[slots[1]].name);
}

ClubController::TeamLabels ClubController::current_set_teams() const {
    if (!round_) {
        return {};
    }
    const domain::ClubPairing pairing = round_->current_pairing();
    return TeamLabels{label_for(pairing.team_a), label_for(pairing.team_b)};
}

void ClubController::on_set_complete(const domain::MatchState& state) {
    if (!round_ || !state.winner || round_->stage() == domain::ClubStage::Complete) {
        return;
    }
    // Club mini-set = single set; the final score is the one completed set.
    const domain::SetScore& score =
        state.completed_set_count > 0 ? state.completed_sets[0] : state.current_set;
    const std::uint8_t games_a = score.games_a;
    const std::uint8_t games_b = score.games_b;
    const bool a_won = *state.winner == TeamId::A;
    const domain::ClubPairing pairing = round_->current_pairing();
    const int set_index = round_->stage() == domain::ClubStage::Set1 ? 1 : 2;
    round_->record_set_result(*state.winner, a_won ? games_a : games_b,
                              a_won ? games_b : games_a);
    last_set_summary_ = label_for(a_won ? pairing.team_a : pairing.team_b) + " took set " +
                        std::to_string(set_index) + " (" +
                        std::to_string(a_won ? games_a : games_b) + "-" +
                        std::to_string(a_won ? games_b : games_a) + ")";
    logging::emit(logging::Level::Info, "club.set_recorded", "set=%d winner=%c %u-%u",
                  round_->stage() == domain::ClubStage::Set2 ? 1 : 2, a_won ? 'A' : 'B',
                  a_won ? games_a : games_b, a_won ? games_b : games_a);

    if (round_->stage() == domain::ClubStage::Complete) {
        const std::uint64_t now = clock_.now_ms();
        const auto top = round_->top2();
        for (const domain::ClubStanding& standing : round_->standings()) {
            const Player& player = players_[standing.slot];
            RoundResult result{};
            result.player_id = player.id;
            result.player_name = player.name;
            result.guest = player.guest;
            result.wins = standing.wins;
            result.differential = standing.differential;
            result.top2 = standing.slot == top[0] || standing.slot == top[1];
            result.decided_by_coin_flip =
                round_->decided_by_coin_flip() && standing.slot == round_->coin_flip_winner();
            result.timestamp_ms = now;
            results_.append(result);
        }
        logging::emit(logging::Level::Info, "club.round_complete", "top2=%s coin=%d",
                      label_for(top).c_str(), round_->decided_by_coin_flip() ? 1 : 0);
    }
}

std::vector<ClubController::StandingRow> ClubController::standings() const {
    std::vector<StandingRow> rows;
    if (!round_ || round_->stage() != domain::ClubStage::Complete) {
        return rows;
    }
    const auto top = round_->top2();
    for (const domain::ClubStanding& standing : round_->standings()) {
        StandingRow row{};
        row.player = players_[standing.slot];
        row.wins = standing.wins;
        row.differential = standing.differential;
        row.top2 = standing.slot == top[0] || standing.slot == top[1];
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string ClubController::coin_flip_announcement() const {
    if (!round_ || round_->stage() != domain::ClubStage::Complete ||
        !round_->decided_by_coin_flip()) {
        return {};
    }
    return "COIN FLIP: " + uppercased(players_[round_->coin_flip_winner()].name) +
           " takes the last TOP 2 spot";
}

void ClubController::finish_round() {
    if (round_ && round_->stage() == domain::ClubStage::Complete) {
        const auto top = round_->top2();
        forbidden_ids_ = {players_[top[0]].id, players_[top[1]].id};
    }
    round_.reset();
}

}  // namespace padel::application
