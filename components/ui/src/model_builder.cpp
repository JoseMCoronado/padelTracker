#include "padel/ui/model_builder.hpp"

#include <cstdio>

#include "padel/domain/projection.hpp"
#include "padel/domain/reducer.hpp"

namespace padel::ui {
namespace {

// A remote counts as "ok" if it has ever reported in and was seen within
// this window (presses are minutes apart in real play, so be generous).
constexpr std::uint64_t kRemoteRecentWindowMs = 30 * 60 * 1000;

std::string team_display_name(const std::string& configured, TeamId team) {
    if (!configured.empty()) {
        return configured;
    }
    return team == TeamId::A ? "TEAM A" : "TEAM B";
}

}  // namespace

const std::vector<std::string>& preset_names() {
    static const std::vector<std::string> names = {
        "Standard advantage",
        "Standard golden point",
        "Mini-set first to 3",
        "Match tiebreak final",
    };
    return names;
}

domain::MatchConfig preset_config(int preset_index) {
    switch (preset_index) {
        case 1:
            return domain::preset_standard_golden_point();
        case 2:
            return domain::preset_club_mini_set();
        case 3:
            return domain::preset_match_tiebreak_final();
        case 0:
        default:
            return domain::preset_standard_advantage();
    }
}

std::string mode_label(const domain::MatchConfig& config) {
    if (config.sets_to_win == 1 && config.normal_set.games_to_win == 3) {
        return "MINI-SET / GP";
    }
    std::string label =
        config.game_rule == domain::GameRule::GoldenPoint ? "STANDARD / GP" : "STANDARD / ADV";
    if (config.final_set_rule == domain::FinalSetRule::MatchTiebreak) {
        label += " / MTB";
    }
    return label;
}

LiveViewModel build_live_model(const application::CourtService& service,
                               const MatchSettings& settings,
                               std::uint64_t now_ms) {
    const domain::MatchState& state = service.state();
    const domain::DisplayState d = domain::project(state);

    LiveViewModel model{};
    model.court_label = settings.court_label;
    model.mode_label = mode_label(state.config);
    model.paused = state.lifecycle == domain::MatchLifecycle::Paused;
    model.status_label = model.paused ? "PAUSED" : "LIVE";
    model.revision = state.revision;
    model.storage_fault = service.storage_fault();
    model.conflict = service.conflict_pending();

    if (d.is_golden_point) {
        model.special_label = "GOLDEN POINT";
    } else if (d.is_deuce) {
        model.special_label = "DEUCE";
    } else if (d.is_match_tiebreak) {
        model.special_label = "MATCH TIEBREAK";
    } else if (d.is_tiebreak) {
        model.special_label = "TIEBREAK";
    }

    const bool serving_enabled = state.config.track_serving_team;
    const auto fill_team = [&](TeamPanelModel& panel, TeamId team) {
        panel.name = team_display_name(team == TeamId::A ? settings.team_a_name
                                                         : settings.team_b_name,
                                       team);
        panel.players = team == TeamId::A ? settings.players_a : settings.players_b;
        panel.points = team == TeamId::A ? d.points_a : d.points_b;
        panel.games = std::to_string(team == TeamId::A ? d.games_a : d.games_b);
        panel.sets = std::to_string(team == TeamId::A ? d.sets_a : d.sets_b);
        panel.serving = serving_enabled && d.serving_team == team &&
                        state.lifecycle != domain::MatchLifecycle::NotStarted;
        if (const auto info = service.remote_info(team)) {
            panel.remote_assigned = true;
            panel.remote_ok = info->ever_seen &&
                              now_ms - info->last_seen_ms < kRemoteRecentWindowMs;
        }
    };
    fill_team(model.team_a, TeamId::A);
    fill_team(model.team_b, TeamId::B);

    // Radio health for the header: OK when every assigned remote is healthy
    // (no remotes assigned counts as OK for local-input-only matches).
    model.radio_ok = (!model.team_a.remote_assigned || model.team_a.remote_ok) &&
                     (!model.team_b.remote_assigned || model.team_b.remote_ok);

    std::string history;
    for (const std::string& set : d.set_history) {
        if (!history.empty()) {
            history += "  ";
        }
        history += set;
    }
    model.set_history = history.empty()
                            ? "current " + model.team_a.games + "-" + model.team_b.games
                            : history + "  |  current " + model.team_a.games + "-" +
                                  model.team_b.games;

    if (serving_enabled && state.lifecycle != domain::MatchLifecycle::NotStarted) {
        model.serving_label =
            "Serving: " + (d.serving_team == TeamId::A ? model.team_a.name : model.team_b.name);
    }

    if (const auto target = service.next_undo_target()) {
        model.undo_preview = target->team;
    }
    return model;
}

CompleteViewModel build_complete_model(const application::CourtService& service,
                                       const MatchSettings& settings,
                                       std::uint64_t match_duration_ms) {
    const domain::DisplayState d = domain::project(service.state());
    CompleteViewModel model{};

    if (d.winner) {
        const std::string name = *d.winner == TeamId::A
                                     ? team_display_name(settings.team_a_name, TeamId::A)
                                     : team_display_name(settings.team_b_name, TeamId::B);
        model.winner_label = name + " WINS";
    } else {
        model.winner_label = "MATCH FINISHED";
    }

    std::string score;
    for (const std::string& set : d.set_history) {
        if (!score.empty()) {
            score += "  ";
        }
        score += set;
    }
    model.final_score = score.empty() ? "-" : score;

    if (match_duration_ms > 0) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "Duration: %llu min",
                      static_cast<unsigned long long>(match_duration_ms / 60000));
        model.duration_label = buffer;
    }
    return model;
}

}  // namespace padel::ui
