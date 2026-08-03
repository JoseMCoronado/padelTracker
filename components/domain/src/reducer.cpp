#include "padel/domain/reducer.hpp"

namespace padel::domain {
namespace {

std::uint8_t& points_of(GameState& game, TeamId team) {
    return team == TeamId::A ? game.raw_points_a : game.raw_points_b;
}

std::uint8_t& games_of(SetScore& set, TeamId team) {
    return team == TeamId::A ? set.games_a : set.games_b;
}

std::uint8_t& tiebreak_points_of(MatchState& state, TeamId team) {
    return team == TeamId::A ? state.tiebreak_points_a : state.tiebreak_points_b;
}

std::uint8_t& sets_won_of(MatchState& state, TeamId team) {
    return team == TeamId::A ? state.sets_won_a : state.sets_won_b;
}

void enter_tiebreak(MatchState& state, bool match_tiebreak) {
    state.in_tiebreak = true;
    state.in_match_tiebreak = match_tiebreak;
    state.tiebreak_points_a = 0;
    state.tiebreak_points_b = 0;
    state.current_game = GameState{};
}

void on_set_won(MatchState& state, TeamId winner) {
    SetScore completed = state.current_set;
    if (state.in_tiebreak) {
        completed.tiebreak_points_a = state.tiebreak_points_a;
        completed.tiebreak_points_b = state.tiebreak_points_b;
    }
    if (state.completed_set_count < kMaxSets) {
        state.completed_sets[state.completed_set_count] = completed;
        ++state.completed_set_count;
    }

    sets_won_of(state, winner) += 1;
    state.current_set = SetScore{};
    state.current_game = GameState{};
    state.in_tiebreak = false;
    state.in_match_tiebreak = false;
    state.tiebreak_points_a = 0;
    state.tiebreak_points_b = 0;

    if (sets_won_of(state, winner) >= state.config.sets_to_win) {
        state.lifecycle = MatchLifecycle::Completed;
        state.winner = winner;
        return;
    }

    if (deciding_set_is_match_tiebreak(state)) {
        enter_tiebreak(state, /*match_tiebreak=*/true);
    }
}

void on_game_won(MatchState& state, TeamId winner) {
    games_of(state.current_set, winner) += 1;
    state.current_game = GameState{};
    // Serving team alternates at game boundaries (spec section 9.6).
    state.serving_team = opponent(state.serving_team);

    const SetRule& rule = state.config.normal_set;
    const std::uint8_t won = games_of(state.current_set, winner);
    const std::uint8_t lost = games_of(state.current_set, opponent(winner));

    const bool reached_target = won >= rule.games_to_win;
    const bool margin_ok = !rule.win_by_two_games || (won >= lost + 2);
    if (reached_target && margin_ok) {
        on_set_won(state, winner);
        return;
    }

    if (rule.tiebreak_enabled && state.current_set.games_a == rule.tiebreak_at_games &&
        state.current_set.games_b == rule.tiebreak_at_games) {
        enter_tiebreak(state, /*match_tiebreak=*/false);
    }
}

void on_tiebreak_point(MatchState& state, TeamId team) {
    tiebreak_points_of(state, team) += 1;

    const bool match_tb = state.in_match_tiebreak;
    const std::uint8_t target = match_tb ? state.config.match_tiebreak_points_to_win
                                         : state.config.normal_set.tiebreak_points_to_win;
    const bool win_by_two = match_tb ? state.config.match_tiebreak_win_by_two
                                     : state.config.normal_set.tiebreak_win_by_two;

    const std::uint8_t won = tiebreak_points_of(state, team);
    const std::uint8_t lost = tiebreak_points_of(state, opponent(team));
    if (won >= target && (!win_by_two || won >= lost + 2)) {
        // Tiebreak winner takes the set's final game (7-6); a match tiebreak
        // is recorded as a 1-0 set carrying the tiebreak points.
        games_of(state.current_set, team) += 1;
        state.serving_team = opponent(state.serving_team);
        on_set_won(state, team);
    }
}

void on_point_awarded(MatchState& state, TeamId team) {
    if (state.in_tiebreak) {
        on_tiebreak_point(state, team);
        return;
    }

    points_of(state.current_game, team) += 1;

    const std::uint8_t won = points_of(state.current_game, team);
    const std::uint8_t lost = points_of(state.current_game, opponent(team));

    bool game_won = false;
    switch (state.config.game_rule) {
        case GameRule::Advantage:
            game_won = won >= 4 && won >= lost + 2;
            break;
        case GameRule::GoldenPoint:
            // Deuce never cycles: 40-40 is decided by the next point, so the
            // golden-point game is exactly "first to 4 raw points".
            game_won = won >= 4;
            break;
    }
    if (game_won) {
        on_game_won(state, team);
    }
}

struct Applier {
    MatchState& state;

    void operator()(const MatchCreated& e) {
        MatchState fresh{};
        fresh.match_id = e.match_id;
        fresh.config = e.config;
        fresh.revision = state.revision;
        state = fresh;
    }

    void operator()(const MatchStarted& e) {
        state.lifecycle = MatchLifecycle::Active;
        state.serving_team = e.initial_serving_team;
        // Degenerate custom config: a one-set match whose final set is a
        // match tiebreak starts directly in the tiebreak.
        if (deciding_set_is_match_tiebreak(state)) {
            enter_tiebreak(state, /*match_tiebreak=*/true);
        }
    }

    void operator()(const PointAwarded& e) { on_point_awarded(state, e.team); }

    void operator()(const ScoringActionUndone&) {
        // Compensation is realized at replay time by skipping the referenced
        // event (ADR-0004); applying the marker itself changes nothing.
    }

    void operator()(const ServingTeamChanged& e) { state.serving_team = e.team; }

    void operator()(const MatchPaused&) { state.lifecycle = MatchLifecycle::Paused; }

    void operator()(const MatchResumed&) { state.lifecycle = MatchLifecycle::Active; }

    void operator()(const MatchFinishedManually& e) {
        state.lifecycle = MatchLifecycle::Completed;
        state.winner = e.declared_winner;
    }

    void operator()(const MatchReset&) {
        MatchState fresh{};
        fresh.match_id = state.match_id;
        fresh.config = state.config;
        fresh.revision = state.revision;
        state = fresh;
    }
};

}  // namespace

bool deciding_set_is_match_tiebreak(const MatchState& state) {
    if (state.config.final_set_rule != FinalSetRule::MatchTiebreak) {
        return false;
    }
    const std::uint8_t need = state.config.sets_to_win;
    return need > 0 && state.sets_won_a == need - 1 && state.sets_won_b == need - 1;
}

MatchState apply(MatchState state, const Event& event) {
    std::visit(Applier{state}, event);
    state.revision += 1;
    return state;
}

MatchConfig preset_standard_advantage() {
    return MatchConfig{};
}

MatchConfig preset_standard_golden_point() {
    MatchConfig config{};
    config.game_rule = GameRule::GoldenPoint;
    return config;
}

MatchConfig preset_club_mini_set() {
    MatchConfig config{};
    config.game_rule = GameRule::GoldenPoint;
    config.sets_to_win = 1;
    config.normal_set.games_to_win = 3;
    config.normal_set.win_by_two_games = false;
    config.normal_set.tiebreak_enabled = false;
    return config;
}

MatchConfig preset_match_tiebreak_final() {
    MatchConfig config{};
    config.final_set_rule = FinalSetRule::MatchTiebreak;
    return config;
}

}  // namespace padel::domain
