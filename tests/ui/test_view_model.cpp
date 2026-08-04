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

TEST_CASE("preset names and configs stay index-aligned") {
    REQUIRE(ui::preset_names().size() == 4);
    CHECK(ui::preset_config(0).game_rule == domain::GameRule::Advantage);
    CHECK(ui::preset_config(1).game_rule == domain::GameRule::GoldenPoint);
    CHECK(ui::preset_config(2).normal_set.games_to_win == 3);
    CHECK(ui::preset_config(3).final_set_rule == domain::FinalSetRule::MatchTiebreak);
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
    CHECK(model.serving_label == "Serving: LOS TIGRES");
    CHECK(model.status_label == "LIVE");
    CHECK_FALSE(model.paused);
    CHECK_FALSE(model.conflict);
    CHECK_FALSE(model.storage_fault);
    REQUIRE(model.undo_preview.has_value());
    CHECK(*model.undo_preview == TeamId::A);
    CHECK(model.set_history == "current 0-0");
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
