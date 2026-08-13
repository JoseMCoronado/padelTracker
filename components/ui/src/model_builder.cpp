#include "padel/ui/model_builder.hpp"

#include <cctype>
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

// The scoreboard is read from across the court, so it prefers the player
// names the crowd recognises over the configured team name.
std::string scoreboard_name(const MatchSettings& settings, TeamId team) {
    const std::string& players = team == TeamId::A ? settings.players_a : settings.players_b;
    if (!players.empty()) {
        return players;
    }
    return team_display_name(team == TeamId::A ? settings.team_a_name : settings.team_b_name,
                             team);
}

// One side of a pair label, cut to the three capitals a plate has room for.
// Only the first word counts, so "Maximiliano Alejandro" reads MAX.
std::string short_name_token(const std::string& name) {
    std::string result;
    for (const unsigned char c : name) {
        if (std::isspace(c) != 0) {
            if (!result.empty()) {
                break;
            }
            continue;
        }
        result.push_back(static_cast<char>(std::toupper(c)));
        if (result.size() == 3) {
            break;
        }
    }
    return result;
}

ScoreColumn set_column(std::uint8_t games_a, std::uint8_t games_b, TeamId winner) {
    ScoreColumn column{};
    column.games_a = std::to_string(static_cast<unsigned>(games_a));
    column.games_b = std::to_string(static_cast<unsigned>(games_b));
    column.won = winner;
    return column;
}

ScoreboardModel build_scoreboard(const domain::DisplayState& display,
                                 const MatchSettings& settings,
                                 bool show_serving) {
    ScoreboardModel board{};
    board.name_a = scoreboard_short_name(scoreboard_name(settings, TeamId::A));
    board.name_b = scoreboard_short_name(scoreboard_name(settings, TeamId::B));
    if (show_serving) {
        board.serving = display.serving_team;
    }

    for (const domain::SetLine& set : display.sets) {
        ScoreColumn column{};
        column.games_a = std::to_string(set.games_a);
        column.games_b = std::to_string(set.games_b);
        // Only the loser's tiebreak points are worth the space, exactly as
        // the broadcast overlays print 7-6(5).
        if (set.tiebreak_points_a && set.tiebreak_points_b) {
            if (*set.tiebreak_points_a < *set.tiebreak_points_b) {
                column.tiebreak_a = "(" + std::to_string(*set.tiebreak_points_a) + ")";
            } else {
                column.tiebreak_b = "(" + std::to_string(*set.tiebreak_points_b) + ")";
            }
        }
        column.current = !set.completed;
        column.won = set.winner;
        board.columns.push_back(std::move(column));
    }
    if (board.columns.empty()) {
        board.columns.push_back(ScoreColumn{"0", "0", "", "", true, std::nullopt});
    }
    return board;
}

}  // namespace

const std::vector<std::string>& preset_names() {
    static const std::vector<std::string> names = {
        "Standard advantage",
        "Standard golden point",
        "Mini-set first to 3",
        "Match tiebreak final",
        "Club round (2x first to 3, mix)",
    };
    return names;
}

domain::MatchConfig preset_config(int preset_index) {
    switch (preset_index) {
        case 1:
            return domain::preset_standard_golden_point();
        case 2:
        case kClubRoundPreset:  // each club set is a mini-set
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

std::string scoreboard_short_name(const std::string& label) {
    std::vector<std::string> sides;
    std::string current;
    for (const char c : label) {
        if (c == '&' || c == '/') {
            sides.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    sides.push_back(current);
    if (sides.size() < 2) {
        return label;  // a club or team name, not a pair
    }

    std::string result;
    for (const std::string& side : sides) {
        const std::string token = short_name_token(side);
        if (token.empty()) {
            return label;  // a stray separator, not a pair after all
        }
        if (!result.empty()) {
            result += "/";
        }
        result += token;
    }
    return result;
}

std::vector<ScoreboardModel> build_club_prior_boards(
    const application::ClubController& controller, int displayed_set_number) {
    std::vector<ScoreboardModel> boards;
    const std::vector<application::ClubController::SetScoreline> played =
        controller.recorded_sets();
    // The set on screen is drawn by the live board; everything before it gets
    // a block of its own. Right after set 1 the round has already moved on to
    // set 2, which is why the caller says which set is showing.
    const std::size_t prior = displayed_set_number > 0
                                  ? static_cast<std::size_t>(displayed_set_number - 1)
                                  : 0;
    for (std::size_t i = 0; i < prior && i < played.size(); ++i) {
        ScoreboardModel board{};
        board.name_a = scoreboard_short_name(played[i].team_a);
        board.name_b = scoreboard_short_name(played[i].team_b);
        board.columns.push_back(
            set_column(played[i].games_a, played[i].games_b, played[i].winner));
        boards.push_back(std::move(board));
    }
    return boards;
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

    model.scoreboard = build_scoreboard(
        d, settings,
        serving_enabled && state.lifecycle != domain::MatchLifecycle::NotStarted);

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

SummaryViewModel build_summary_model(const application::CourtService& service,
                                     const MatchSettings& settings,
                                     std::uint64_t match_duration_ms,
                                     const std::string& title_override,
                                     const std::string& continue_label) {
    const domain::MatchState& state = service.state();
    const domain::DisplayState d = domain::project(state);
    const domain::MatchStats stats = domain::summarize(service.journal());

    SummaryViewModel model{};
    model.title = title_override.empty() ? "MATCH COMPLETE" : title_override;
    model.continue_label = continue_label;
    model.scoreboard = build_scoreboard(d, settings, /*show_serving=*/false);

    const std::string name_a = scoreboard_name(settings, TeamId::A);
    const std::string name_b = scoreboard_name(settings, TeamId::B);
    if (d.winner) {
        model.winner_label = (*d.winner == TeamId::A ? name_a : name_b) + " WIN";
    } else {
        model.winner_label = "NO WINNER RECORDED";
    }

    char buffer[64];
    if (match_duration_ms > 0) {
        std::snprintf(buffer, sizeof(buffer), "%llu min",
                      static_cast<unsigned long long>(match_duration_ms / 60000));
        model.stats.push_back({"Duration", buffer});
    }

    // The counts are uint32_t, which is unsigned long on xtensa, so every one
    // of them is cast to unsigned for the format string.
    const std::uint32_t total = stats.points_a + stats.points_b;
    std::snprintf(buffer, sizeof(buffer), "%u", static_cast<unsigned>(total));
    model.stats.push_back({"Points played", buffer});
    if (total > 0) {
        const auto share = [&](std::uint32_t points) {
            return static_cast<unsigned>((points * 100 + total / 2) / total);
        };
        std::snprintf(buffer, sizeof(buffer), "%u  (%u%%)",
                      static_cast<unsigned>(stats.points_a), share(stats.points_a));
        model.stats.push_back({name_a, buffer});
        std::snprintf(buffer, sizeof(buffer), "%u  (%u%%)",
                      static_cast<unsigned>(stats.points_b), share(stats.points_b));
        model.stats.push_back({name_b, buffer});
    }
    if (stats.longest_streak > 1 && stats.longest_streak_team) {
        std::snprintf(buffer, sizeof(buffer), "%u in a row",
                      static_cast<unsigned>(stats.longest_streak));
        model.stats.push_back(
            {std::string("Best run - ") +
                 (*stats.longest_streak_team == TeamId::A ? name_a : name_b),
             buffer});
    }
    return model;
}

ClubViewModel build_club_model(const application::PlayerRoster& roster,
                               const application::ClubController& controller,
                               const std::string& setup_hint) {
    ClubViewModel model{};
    model.setup_hint = setup_hint;
    for (const application::Player& player : roster.players()) {
        model.roster.push_back(ClubPlayer{player.id, player.name, player.guest});
    }

    if (!controller.round_active()) {
        return model;
    }

    if (controller.stage() == domain::ClubStage::Set2) {
        const auto teams = controller.current_set_teams();
        model.mix_detail = controller.last_set_summary();
        model.mix_team_a = teams.team_a;
        model.mix_team_b = teams.team_b;
    } else if (controller.stage() == domain::ClubStage::Complete) {
        int rank = 0;
        for (const auto& row : controller.standings()) {
            ClubStandingRowModel entry{};
            entry.rank = std::to_string(++rank);
            entry.name = row.player.name;
            char record[32];
            std::snprintf(record, sizeof(record), "%u %s  %+d", row.wins,
                          row.wins == 1 ? "WIN" : "WINS", row.differential);
            entry.record = record;
            entry.top2 = row.top2;
            model.standings.push_back(std::move(entry));
        }
        model.coin_announcement = controller.coin_flip_announcement();
        // The flip decides the second Top 2 spot (rank 2).
        if (!model.coin_announcement.empty() && model.standings.size() > 1) {
            model.standings[1].coin = true;
        }
    }
    return model;
}

std::vector<application::ClubController::ForbiddenPair> crown_pairs(
    const std::array<ClubPlayer, 4>& picked) {
    std::vector<application::ClubController::ForbiddenPair> pairs;
    for (std::uint8_t crown = 1; crown <= kMaxCrownGroups; ++crown) {
        std::vector<std::uint32_t> wearers;
        for (const ClubPlayer& player : picked) {
            if (player.crown == crown) {
                wearers.push_back(player.id);
            }
        }
        if (wearers.size() == 2) {
            pairs.push_back({wearers[0], wearers[1]});
        }
    }
    return pairs;
}

bool suggest_next_round_picks(const application::ClubController& controller,
                              std::vector<ClubPlayer>& out_a,
                              std::vector<ClubPlayer>& out_b) {
    out_a.clear();
    out_b.clear();
    if (!controller.round_active() ||
        controller.stage() != domain::ClubStage::Complete) {
        return false;
    }
    std::vector<ClubPlayer> top;
    for (const auto& row : controller.standings()) {
        if (row.top2) {
            top.push_back(ClubPlayer{row.player.id, row.player.name, row.player.guest});
        }
    }
    if (top.size() != 2) {
        return false;
    }
    // Rank-1 Top 2 alone on Team A, rank-2 alone on Team B.
    out_a.push_back(top[0]);
    out_b.push_back(top[1]);
    return true;
}

}  // namespace padel::ui
