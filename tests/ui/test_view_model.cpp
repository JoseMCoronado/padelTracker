// View-model projection tests (spec 14 / 18.6): the UI layer's pure logic,
// exercised against a real CourtService.
#include <catch2/catch_test_macros.hpp>

#include "../application/fakes.hpp"
#include "padel/domain/types.hpp"
#include "padel/ui/model_builder.hpp"

using namespace padel;
using namespace padel::application;
using namespace padel::application::testing;

namespace {

constexpr RemoteId kRemoteA = 0xA1;

protocol::PointIntentPacket intent(RemoteId remote, std::uint32_t seq, TeamId team) {
    protocol::PointIntentPacket packet{};
    packet.court_id = 1;
    packet.identity = protocol::IntentIdentity{remote, 0x1234, seq};
    packet.team = team;
    packet.battery_mv = 3900;
    return packet;
}

struct Fixture {
    FakeClock clock{};
    FakeEventStore store{};
    CourtService service{CourtServiceConfig{1, 0}, domain::preset_standard_golden_point(), store,
                         clock};
    ui::MatchSettings settings{};
};

}  // namespace

namespace {

class MemoryRosterStore : public application::IRosterStore {
public:
    std::vector<Player> load() override { return {}; }
    bool save(const std::vector<Player>&) override { return true; }
};

class NullResultsLog : public application::IResultsLog {
public:
    bool append(const application::RoundResult&) override { return true; }
};

domain::MatchState club_set_state(TeamId winner, std::uint8_t loser_games) {
    domain::MatchState state{};
    state.lifecycle = domain::MatchLifecycle::Completed;
    state.winner = winner;
    state.completed_set_count = 1;
    state.completed_sets[0].games_a = winner == TeamId::A ? 3 : loser_games;
    state.completed_sets[0].games_b = winner == TeamId::B ? 3 : loser_games;
    return state;
}

}  // namespace

TEST_CASE("club model: roster tiles, mix labels, standings, coin row") {
    MemoryRosterStore store;
    NullResultsLog log;
    FakeClock clock;
    PlayerRoster roster(store);  // seeds the club regulars
    ClubController controller(log, clock);

    ui::ClubViewModel idle = ui::build_club_model(roster, controller, "split them up");
    REQUIRE(idle.roster.size() == 12);
    CHECK(idle.setup_hint == "split them up");
    CHECK(idle.mix_team_a.empty());
    CHECK(idle.standings.empty());

    const auto find = [&](const char* name) {
        for (const auto& player : roster.players()) {
            if (player.name == name) {
                return player;
            }
        }
        FAIL("missing seeded player: " << name);
        return application::Player{};
    };
    REQUIRE_FALSE(controller
                      .start_round({find("Adrien"), find("Lewis"), find("Louis"), find("Luigi")}, 0)
                      .has_value());
    controller.on_set_complete(club_set_state(TeamId::A, 2));

    ui::ClubViewModel mix = ui::build_club_model(roster, controller, "");
    CHECK(mix.mix_detail == "ADRIEN & LEWIS took set 1 (3-2)");
    CHECK(mix.mix_team_a == "ADRIEN & LOUIS");
    CHECK(mix.mix_team_b == "LEWIS & LUIGI");

    // Set 2 also ends 3-2, the other way: the 1-win players (Adrien, Luigi)
    // tie on differential -> automatic coin flip on the rank-2 row.
    controller.on_set_complete(club_set_state(TeamId::B, 2));
    ui::ClubViewModel done = ui::build_club_model(roster, controller, "");
    REQUIRE(done.standings.size() == 4);
    CHECK(done.standings[0].name == "Lewis");
    CHECK(done.standings[0].record.find("2 WINS") != std::string::npos);
    CHECK(done.standings[0].top2);
    CHECK(done.standings[1].top2);
    CHECK_FALSE(done.standings[2].top2);
    REQUIRE_FALSE(done.coin_announcement.empty());
    CHECK(done.standings[1].coin);

    // NEW ROUND seed: only Top 2, one per team; partners left empty.
    std::vector<ui::ClubPlayer> suggested_a;
    std::vector<ui::ClubPlayer> suggested_b;
    REQUIRE(ui::suggest_next_round_picks(controller, suggested_a, suggested_b));
    REQUIRE(suggested_a.size() == 1);
    REQUIRE(suggested_b.size() == 1);
    CHECK(suggested_a[0].name == "Lewis");                 // rank-1 Top 2
    CHECK(suggested_b[0].name == done.standings[1].name);  // rank-2 Top 2
    CHECK(suggested_a[0].id != suggested_b[0].id);

    controller.finish_round();
    CHECK_FALSE(ui::suggest_next_round_picks(controller, suggested_a, suggested_b));
}

TEST_CASE("scoreboard cuts pair labels to three capitals a side") {
    CHECK(ui::scoreboard_short_name("JOSE & RUXANDRA") == "JOS/RUX");
    CHECK(ui::scoreboard_short_name("Jose / Zoe") == "JOS/ZOE");
    CHECK(ui::scoreboard_short_name("Maximiliano Alejandro & Sebastian Rodriguez") == "MAX/SEB");
    CHECK(ui::scoreboard_short_name("Al & Bo") == "AL/BO");  // shorter than three

    // Not a pair: a club or team name is left alone rather than mangled.
    CHECK(ui::scoreboard_short_name("LOS TIGRES") == "LOS TIGRES");
    CHECK(ui::scoreboard_short_name("TEAM A") == "TEAM A");
    CHECK(ui::scoreboard_short_name("LOS TIGRES &") == "LOS TIGRES &");
}

TEST_CASE("club prior boards keep each mini-set with the pairing that played it") {
    MemoryRosterStore store;
    NullResultsLog log;
    FakeClock clock;
    PlayerRoster roster(store);
    ClubController controller(log, clock);

    const auto find = [&](const char* name) {
        for (const auto& player : roster.players()) {
            if (player.name == name) {
                return player;
            }
        }
        FAIL("missing seeded player: " << name);
        return application::Player{};
    };
    REQUIRE_FALSE(controller
                      .start_round({find("Adrien"), find("Lewis"), find("Louis"), find("Luigi")}, 0)
                      .has_value());
    CHECK(ui::build_club_prior_boards(controller, 1).empty());

    controller.on_set_complete(club_set_state(TeamId::A, 1));  // ADRIEN & LEWIS 3-1
    // The set-1 summary still shows set 1 itself, so nothing sits beside it.
    CHECK(ui::build_club_prior_boards(controller, 1).empty());

    const std::vector<ui::ScoreboardModel> boards = ui::build_club_prior_boards(controller, 2);
    REQUIRE(boards.size() == 1);
    CHECK(boards[0].name_a == "ADR/LEW");
    CHECK(boards[0].name_b == "LOU/LUI");
    CHECK_FALSE(boards[0].serving.has_value());
    REQUIRE(boards[0].columns.size() == 1);
    CHECK(boards[0].columns[0].games_a == "3");
    CHECK(boards[0].columns[0].games_b == "1");
    CHECK_FALSE(boards[0].columns[0].current);
    REQUIRE(boards[0].columns[0].won.has_value());
    CHECK(*boards[0].columns[0].won == TeamId::A);

    // Set 2 is played by the mixed pairing, and set 1 stays as it was.
    controller.on_set_complete(club_set_state(TeamId::B, 0));
    const std::vector<ui::ScoreboardModel> after = ui::build_club_prior_boards(controller, 2);
    REQUIRE(after.size() == 1);
    CHECK(after[0].name_a == "ADR/LEW");
}

TEST_CASE("preset names and configs stay index-aligned") {
    REQUIRE(ui::preset_names().size() == 5);
    CHECK(ui::preset_config(0).game_rule == domain::GameRule::Advantage);
    CHECK(ui::preset_config(1).game_rule == domain::GameRule::GoldenPoint);
    CHECK(ui::preset_config(2).normal_set.games_to_win == 3);
    CHECK(ui::preset_config(3).final_set_rule == domain::FinalSetRule::MatchTiebreak);
    // Club round mode scores each set as the club mini-set.
    CHECK(ui::preset_config(ui::kClubRoundPreset).normal_set.games_to_win == 3);
    CHECK(ui::preset_config(ui::kClubRoundPreset).sets_to_win == 1);
}

TEST_CASE("mode labels cover all presets") {
    CHECK(ui::mode_label(ui::preset_config(0)) == "STANDARD / ADV");
    CHECK(ui::mode_label(ui::preset_config(1)) == "STANDARD / GP");
    CHECK(ui::mode_label(ui::preset_config(2)) == "MINI-SET / GP");
    CHECK(ui::mode_label(ui::preset_config(3)) == "STANDARD / ADV / MTB");
}

TEST_CASE("live model projects names, points, serving, and undo preview") {
    Fixture f;
    f.settings.team_a_name = "LOS TIGRES";
    f.settings.team_b_name = "";  // falls back to TEAM B
    f.service.start_match(TeamId::A);
    f.service.award_point_local(TeamId::A, InputSource::TouchscreenAdmin);

    const ui::LiveViewModel model =
        ui::build_live_model(f.service, f.settings, f.clock.now_ms());

    CHECK(model.team_a.name == "LOS TIGRES");
    CHECK(model.team_b.name == "TEAM B");
    CHECK(model.team_a.points == "15");
    CHECK(model.team_b.points == "0");
    CHECK(model.team_a.serving);
    CHECK_FALSE(model.team_b.serving);
    CHECK(model.status_label == "LIVE");
    CHECK_FALSE(model.paused);
    CHECK_FALSE(model.conflict);
    CHECK_FALSE(model.storage_fault);
    REQUIRE(model.undo_preview.has_value());
    CHECK(*model.undo_preview == TeamId::A);

    // The scoreboard names the pair when it can and marks the server.
    CHECK(model.scoreboard.name_a == "LOS TIGRES");
    CHECK(model.scoreboard.name_b == "TEAM B");
    REQUIRE(model.scoreboard.serving.has_value());
    CHECK(*model.scoreboard.serving == TeamId::A);
    REQUIRE(model.scoreboard.columns.size() == 1);
    CHECK(model.scoreboard.columns[0].current);
    CHECK(model.scoreboard.columns[0].games_a == "0");
    CHECK(model.scoreboard.columns[0].games_b == "0");
}

TEST_CASE("scoreboard keeps one column per set with the loser's tiebreak points") {
    Fixture f;
    f.settings.players_a = "JOSE / ZOE";
    f.service.start_match(TeamId::A);
    // Take the first set 6-0 so a second column opens behind it.
    for (int game = 0; game < 6; ++game) {
        for (int point = 0; point < 4; ++point) {
            f.service.award_point_local(TeamId::A, InputSource::TouchscreenAdmin);
        }
    }
    const ui::LiveViewModel model =
        ui::build_live_model(f.service, f.settings, f.clock.now_ms());

    // Player names beat the configured team name on the scoreboard, cut to
    // three capitals a side.
    CHECK(model.scoreboard.name_a == "JOS/ZOE");
    REQUIRE(model.scoreboard.columns.size() == 2);
    CHECK(model.scoreboard.columns[0].games_a == "6");
    CHECK(model.scoreboard.columns[0].games_b == "0");
    CHECK_FALSE(model.scoreboard.columns[0].current);
    REQUIRE(model.scoreboard.columns[0].won.has_value());
    CHECK(*model.scoreboard.columns[0].won == TeamId::A);
    CHECK(model.scoreboard.columns[1].current);
}

TEST_CASE("summary model counts rallies from the journal and skips undone ones") {
    Fixture f;
    f.settings.team_a_name = "LOS TIGRES";
    f.service.start_match(TeamId::A);
    for (int i = 0; i < 5; ++i) {
        f.service.award_point_local(TeamId::A, InputSource::TouchscreenAdmin);
    }
    f.service.award_point_local(TeamId::B, InputSource::TouchscreenAdmin);
    f.service.undo_last_scoring_action();  // that Team B point never happened

    const ui::SummaryViewModel model =
        ui::build_summary_model(f.service, f.settings, 42 * 60 * 1000, "SET 1 COMPLETE",
                                "MIX IT UP");

    CHECK(model.title == "SET 1 COMPLETE");
    CHECK(model.continue_label == "MIX IT UP");

    const auto value_for = [&](const std::string& label) {
        for (const auto& row : model.stats) {
            if (row.first == label) {
                return row.second;
            }
        }
        return std::string{"<missing>"};
    };
    CHECK(value_for("Duration") == "42 min");
    CHECK(value_for("Points played") == "5");
    CHECK(value_for("LOS TIGRES") == "5  (100%)");
    CHECK(value_for("Best run - LOS TIGRES") == "5 in a row");
}

TEST_CASE("live model surfaces special states and pause") {
    Fixture f;
    f.service.start_match(TeamId::A);
    // Reach 40-40 under golden point.
    for (int i = 0; i < 3; ++i) {
        f.service.award_point_local(TeamId::A, InputSource::TouchscreenAdmin);
        f.service.award_point_local(TeamId::B, InputSource::TouchscreenAdmin);
    }
    ui::LiveViewModel model = ui::build_live_model(f.service, f.settings, f.clock.now_ms());
    CHECK(model.special_label == "GOLDEN POINT");

    f.service.pause_match();
    model = ui::build_live_model(f.service, f.settings, f.clock.now_ms());
    CHECK(model.paused);
    CHECK(model.status_label == "PAUSED");
}

TEST_CASE("remote status: unassigned, assigned-unseen, and healthy") {
    Fixture f;
    ui::LiveViewModel model = ui::build_live_model(f.service, f.settings, f.clock.now_ms());
    CHECK_FALSE(model.team_a.remote_assigned);
    CHECK(model.radio_ok);  // no remotes = OK for local-input matches

    f.service.assign_remote(kRemoteA, TeamId::A);
    model = ui::build_live_model(f.service, f.settings, f.clock.now_ms());
    CHECK(model.team_a.remote_assigned);
    CHECK_FALSE(model.team_a.remote_ok);
    CHECK_FALSE(model.radio_ok);

    f.service.start_match(TeamId::A);
    f.service.handle_point_intent(intent(kRemoteA, 1, TeamId::A));
    model = ui::build_live_model(f.service, f.settings, f.clock.now_ms());
    CHECK(model.team_a.remote_ok);
    CHECK(model.radio_ok);
}

TEST_CASE("conflict flag mirrors the pending conflict") {
    FakeClock clock;
    FakeEventStore store;
    CourtService service{CourtServiceConfig{1, 250}, domain::preset_standard_golden_point(),
                         store, clock};
    service.start_match(TeamId::A);
    service.award_point_local(TeamId::A, InputSource::PhysicalBackupButton);
    service.award_point_local(TeamId::B, InputSource::PhysicalBackupButton);
    REQUIRE(service.conflict_pending());

    ui::MatchSettings settings{};
    const ui::LiveViewModel model = ui::build_live_model(service, settings, clock.now_ms());
    CHECK(model.conflict);
}

TEST_CASE("complete model formats winner, score line, and duration") {
    Fixture f;
    f.settings.team_b_name = "LAS AGUILAS";
    f.service.start_match(TeamId::A);
    // Team B wins the mini match quickly under golden point: give B every
    // point until the match completes.
    while (f.service.state().lifecycle != domain::MatchLifecycle::Completed) {
        f.service.award_point_local(TeamId::B, InputSource::TouchscreenAdmin);
    }

    const ui::CompleteViewModel model =
        ui::build_complete_model(f.service, f.settings, 5'400'000);
    CHECK(model.winner_label == "LAS AGUILAS WINS");
    // Set history is always from team A's perspective.
    CHECK(model.final_score == "0-6  0-6");
    CHECK(model.duration_label == "Duration: 90 min");
}
