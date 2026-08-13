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

std::vector<ClubController::ForbiddenPair> ClubController::collect_forbidden(
    const std::vector<ForbiddenPair>& crowned) const {
    std::vector<ForbiddenPair> all;
    const auto add = [&](const ForbiddenPair& pair) {
        if (pair[0] == pair[1] || all.size() >= domain::ClubForbiddenPairs::kMax) {
            return;
        }
        for (const ForbiddenPair& existing : all) {
            if ((existing[0] == pair[0] && existing[1] == pair[1]) ||
                (existing[0] == pair[1] && existing[1] == pair[0])) {
                return;
            }
        }
        all.push_back(pair);
    };
    // The court's own Top 2 first: it is the rule the sheet enforces even when
    // the organizer forgets to crown anybody.
    if (forbidden_ids_) {
        add(*forbidden_ids_);
    }
    for (const ForbiddenPair& pair : crowned) {
        add(pair);
    }
    return all;
}

std::optional<ClubController::StartError> ClubController::start_round(
    const std::array<Player, 4>& players, std::uint32_t coin_seed,
    const std::vector<ForbiddenPair>& crowned) {
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

    const std::vector<ForbiddenPair> forbidden = collect_forbidden(crowned);
    const auto slot_of = [&](std::uint32_t id) -> std::optional<std::uint8_t> {
        for (std::uint8_t slot = 0; slot < 4; ++slot) {
            if (players[slot].id == id) {
                return slot;
            }
        }
        return std::nullopt;
    };

    domain::ClubForbiddenPairs slots{};
    for (const ForbiddenPair& pair : forbidden) {
        const auto first = slot_of(pair[0]);
        const auto second = slot_of(pair[1]);
        if (!first || !second) {
            continue;  // that pair is not on this court tonight
        }
        // Teams are {0,1} and {2,3}: same side means the picks already broke
        // the rule, before the mix ever gets a say.
        if (*first / 2 == *second / 2) {
            return StartError::ForbiddenPair;
        }
        slots.add(*first, *second);
    }

    players_ = players;
    set_summaries_ = {};
    last_set_summary_.clear();
    round_.emplace(coin_seed, slots);
    logging::emit(logging::Level::Info, "club.round_started", "a=%s+%s b=%s+%s barred=%u",
                  players[0].name.c_str(), players[1].name.c_str(), players[2].name.c_str(),
                  players[3].name.c_str(), static_cast<unsigned>(slots.count));
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

std::vector<ClubController::SetScoreline> ClubController::recorded_sets() const {
    std::vector<SetScoreline> lines;
    if (!round_) {
        return lines;
    }
    for (int index = 0; index < round_->recorded_set_count(); ++index) {
        const domain::ClubSetResult set = round_->set_result(index);
        SetScoreline line{};
        line.team_a = label_for(set.pairing.team_a);
        line.team_b = label_for(set.pairing.team_b);
        line.games_a = set.games_a;
        line.games_b = set.games_b;
        line.winner = set.winner;
        lines.push_back(std::move(line));
    }
    return lines;
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
    set_summaries_[static_cast<std::size_t>(set_index - 1)] =
        label_for(a_won ? pairing.team_a : pairing.team_b) + " took set " +
        std::to_string(set_index) + " (" + std::to_string(a_won ? games_a : games_b) + "-" +
        std::to_string(a_won ? games_b : games_a) + ")";
    last_set_summary_ = set_summaries_[static_cast<std::size_t>(set_index - 1)];
    logging::emit(logging::Level::Info, "club.set_recorded", "set=%d winner=%c %u-%u",
                  round_->stage() == domain::ClubStage::Set2 ? 1 : 2, a_won ? 'A' : 'B',
                  a_won ? games_a : games_b, a_won ? games_b : games_a);

    if (round_->stage() == domain::ClubStage::Complete) {
        logging::emit(logging::Level::Info, "club.round_complete", "top2=%s coin=%d",
                      label_for(round_->top2()).c_str(), round_->decided_by_coin_flip() ? 1 : 0);
    }
}

bool ClubController::undo_last_set() {
    if (!round_) {
        return false;
    }
    const std::size_t undone = round_->stage() == domain::ClubStage::Complete ? 1 : 0;
    if (!round_->undo_last_set_result()) {
        return false;
    }
    set_summaries_[undone].clear();
    last_set_summary_ = undone == 1 ? set_summaries_[0] : std::string{};
    logging::emit(logging::Level::Info, "club.set_undone", "set=%d",
                  static_cast<int>(undone) + 1);
    return true;
}

void ClubController::write_results_log() {
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
    // The log is written here rather than on completion so an undo that
    // reopens the last mini-set cannot leave stale rows behind it.
    if (round_ && round_->stage() == domain::ClubStage::Complete) {
        write_results_log();
        const auto top = round_->top2();
        forbidden_ids_ = {players_[top[0]].id, players_[top[1]].id};
    }
    round_.reset();
}

}  // namespace padel::application
