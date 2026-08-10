// Spec section 18.3: application service tests. Every input source funnels
// into CourtService; these tests drive it with a fake clock and fake event
// store to pin down dedup, rejection mapping, storage-failure semantics, ACK
// ordering, and the conflict guard.
#include <catch2/catch_test_macros.hpp>

#include "fakes.hpp"
#include "padel/application/court_service.hpp"
#include "padel/domain/reducer.hpp"

using namespace padel;
using namespace padel::application;
using namespace padel::application::testing;

namespace {

constexpr CourtId kCourt = 7;
constexpr RemoteId kRemoteA = 0xA0;
constexpr RemoteId kRemoteB = 0xB0;

protocol::PointIntentPacket intent(RemoteId remote, TeamId team, std::uint32_t seq,
                                   std::uint32_t boot = 1, CourtId court = kCourt) {
    protocol::PointIntentPacket packet{};
    packet.court_id = court;
    packet.identity = protocol::IntentIdentity{remote, boot, seq};
    packet.team = team;
    return packet;
}

protocol::PointIntentPacket undo_intent(RemoteId remote, TeamId team, std::uint32_t seq) {
    protocol::PointIntentPacket packet = intent(remote, team, seq);
    packet.action = protocol::Action::UndoLastPoint;
    return packet;
}

struct Fixture {
    FakeClock clock{};
    FakeEventStore store{};
    CourtService service;

    explicit Fixture(std::uint32_t window_ms = 250)
        : service(CourtServiceConfig{kCourt, window_ms},
                  domain::preset_standard_advantage(), store, clock) {
        service.assign_remote(kRemoteA, TeamId::A);
        service.assign_remote(kRemoteB, TeamId::B);
        REQUIRE(service.start_match(TeamId::A).has_value());
    }

    // Runs the clock past the conflict window and commits pending presses.
    void expire_window() {
        clock.advance(251);
        service.tick();
    }

    std::vector<protocol::AckPacket> acks() { return service.drain_acks(); }

    std::uint8_t points_a() const { return service.state().current_game.raw_points_a; }
    std::uint8_t points_b() const { return service.state().current_game.raw_points_b; }
};

}  // namespace

TEST_CASE("new intent is accepted exactly once, ACK carries new revision") {
    Fixture f;
    const auto before = f.service.state().revision;

    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
    CHECK(f.acks().empty());  // parked in the conflict window, no terminal ACK yet
    CHECK(f.points_a() == 0);

    f.expire_window();
    const auto acks = f.acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].status == protocol::AckStatus::Accepted);
    CHECK(acks[0].identity == protocol::IntentIdentity{kRemoteA, 1, 1});
    CHECK(acks[0].state_revision == before + 1);
    CHECK(f.points_a() == 1);
    CHECK(f.store.point_count() == 1);
}

TEST_CASE("retries of a committed intent get DuplicateAccepted, applied once") {
    Fixture f;
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
    f.expire_window();
    f.acks();

    for (int retry = 0; retry < 3; ++retry) {
        f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
    }
    const auto acks = f.acks();
    REQUIRE(acks.size() == 3);
    for (const auto& ack : acks) {
        CHECK(ack.status == protocol::AckStatus::DuplicateAccepted);
    }
    CHECK(f.points_a() == 1);
    CHECK(f.store.point_count() == 1);
}

TEST_CASE("retry while the press is still parked in the window is silent") {
    Fixture f;
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));  // radio retry
    CHECK(f.acks().empty());

    f.expire_window();
    REQUIRE(f.acks().size() == 1);
    CHECK(f.points_a() == 1);
}

TEST_CASE("remote undo takes back the last point") {
    Fixture f;
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
    f.expire_window();
    f.acks();
    REQUIRE(f.points_a() == 1);

    f.service.handle_point_intent(undo_intent(kRemoteA, TeamId::A, 2));
    const auto acks = f.acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].status == protocol::AckStatus::Accepted);
    CHECK(f.points_a() == 0);
    CHECK(f.service.counters().remote_undos == 1);
}

TEST_CASE("either remote takes back the last point, whoever scored it") {
    // On court nobody wants to work out which button owns the mistake
    // (ADR-0014), so a Team A hold reverses Team B's point.
    Fixture f;
    f.service.handle_point_intent(intent(kRemoteB, TeamId::B, 1));
    f.expire_window();
    f.acks();
    REQUIRE(f.points_b() == 1);

    f.service.handle_point_intent(undo_intent(kRemoteA, TeamId::A, 1));
    const auto acks = f.acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].status == protocol::AckStatus::Accepted);
    CHECK(f.points_b() == 0);
    CHECK(f.service.counters().remote_undos == 1);
}

TEST_CASE("an undo after the winning point reopens the finished match") {
    // The court display walks its own screens back; the service only has to
    // let the undo through and return the match to Active.
    Fixture f;
    std::uint32_t sequence = 0;
    const auto score = [&](std::uint32_t remote, TeamId team) {
        f.service.handle_point_intent(intent(remote, team, ++sequence));
        f.expire_window();
        f.acks();
    };
    // Team A runs the whole match out; the exact count does not matter.
    while (f.service.state().lifecycle != domain::MatchLifecycle::Completed) {
        score(kRemoteA, TeamId::A);
    }
    REQUIRE(f.service.state().winner.has_value());

    f.service.handle_point_intent(undo_intent(kRemoteB, TeamId::B, ++sequence));
    const auto acks = f.acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].status == protocol::AckStatus::Accepted);
    CHECK(f.service.state().lifecycle == domain::MatchLifecycle::Active);
    CHECK_FALSE(f.service.state().winner.has_value());
}

TEST_CASE("remote undo with nothing to undo is rejected, not silently accepted") {
    Fixture f;
    f.service.handle_point_intent(undo_intent(kRemoteA, TeamId::A, 1));
    const auto acks = f.acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].status == protocol::AckStatus::RejectedNothingToUndo);
}

TEST_CASE("a retried undo re-ACKs as duplicate instead of undoing twice") {
    Fixture f;
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
    f.expire_window();
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 2));
    f.expire_window();
    f.acks();
    REQUIRE(f.points_a() == 2);

    f.service.handle_point_intent(undo_intent(kRemoteA, TeamId::A, 3));
    for (int retry = 0; retry < 3; ++retry) {
        f.service.handle_point_intent(undo_intent(kRemoteA, TeamId::A, 3));
    }
    const auto acks = f.acks();
    REQUIRE(acks.size() == 4);
    CHECK(acks[0].status == protocol::AckStatus::Accepted);
    for (std::size_t i = 1; i < acks.size(); ++i) {
        CHECK(acks[i].status == protocol::AckStatus::DuplicateAccepted);
    }
    CHECK(f.points_a() == 1);
}

TEST_CASE("remote undo is refused while a press is parked or a conflict is open") {
    Fixture f;
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
    f.expire_window();
    f.acks();

    // A press is now parked in the window: the score is not settled.
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 2));
    f.service.handle_point_intent(undo_intent(kRemoteA, TeamId::A, 3));
    const auto acks = f.acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].status == protocol::AckStatus::RejectedConflict);
    CHECK(acks[0].identity.sequence == 3);
}

TEST_CASE("remote undo is journaled as a remote-sourced action") {
    Fixture f;
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
    f.expire_window();
    f.acks();

    f.service.handle_point_intent(undo_intent(kRemoteA, TeamId::A, 2));
    REQUIRE(f.acks().size() == 1);
    REQUIRE_FALSE(f.store.events.empty());
    CHECK(f.store.events.back().source == InputSource::Remote);
    CHECK(std::holds_alternative<domain::ScoringActionUndone>(f.store.events.back().payload));
}

TEST_CASE("rejection mapping: unpaired, wrong team, wrong court") {
    Fixture f;

    f.service.handle_point_intent(intent(0xDEAD, TeamId::A, 1));
    f.service.handle_point_intent(intent(kRemoteA, TeamId::B, 1));
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 2, 1, kCourt + 1));

    const auto acks = f.acks();
    REQUIRE(acks.size() == 3);
    CHECK(acks[0].status == protocol::AckStatus::RejectedUnpaired);
    CHECK(acks[1].status == protocol::AckStatus::RejectedWrongTeam);
    CHECK(acks[2].status == protocol::AckStatus::RejectedInvalidPacket);
    CHECK(f.store.point_count() == 0);
}

TEST_CASE("rejection mapping: paused, not started, completed") {
    FakeClock clock;
    FakeEventStore store;

    SECTION("before start -> RejectedNotInMatch") {
        CourtService service(CourtServiceConfig{kCourt, 250},
                             domain::preset_standard_advantage(), store, clock);
        service.assign_remote(kRemoteA, TeamId::A);
        service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
        const auto acks = service.drain_acks();
        REQUIRE(acks.size() == 1);
        CHECK(acks[0].status == protocol::AckStatus::RejectedNotInMatch);
    }

    SECTION("paused -> RejectedPaused") {
        Fixture f;
        REQUIRE(f.service.pause_match().has_value());
        f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
        const auto acks = f.acks();
        REQUIRE(acks.size() == 1);
        CHECK(acks[0].status == protocol::AckStatus::RejectedPaused);
    }

    SECTION("completed -> RejectedNotInMatch") {
        Fixture f;
        REQUIRE(f.service.finish_match_manually(TeamId::B).has_value());
        f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
        const auto acks = f.acks();
        REQUIRE(acks.size() == 1);
        CHECK(acks[0].status == protocol::AckStatus::RejectedNotInMatch);
    }
}

TEST_CASE("pause during the pending window converts the press to RejectedPaused") {
    Fixture f;
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
    REQUIRE(f.service.pause_match().has_value());
    f.expire_window();

    const auto acks = f.acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].status == protocol::AckStatus::RejectedPaused);
    CHECK(f.points_a() == 0);
    CHECK(f.store.point_count() == 0);
}

TEST_CASE("storage failure: ErrorStorage, state unchanged, retry works after recovery") {
    Fixture f(0);  // window disabled: press commits synchronously
    f.store.fail = true;

    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
    auto acks = f.acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].status == protocol::AckStatus::ErrorStorage);
    CHECK(f.points_a() == 0);
    CHECK(f.service.storage_fault());
    CHECK(f.service.counters().storage_failures == 1);

    // The failed intent was never recorded in dedup: the retry is processed
    // as new once storage recovers, and the point lands exactly once.
    f.store.fail = false;
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
    acks = f.acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].status == protocol::AckStatus::Accepted);
    CHECK(f.points_a() == 1);
    CHECK(f.store.point_count() == 1);
}

TEST_CASE("Accepted is only ACKed after the durable append") {
    FakeClock clock;
    FakeEventStore store;
    CourtService service(CourtServiceConfig{kCourt, 0},
                         domain::preset_standard_advantage(), store, clock);
    service.assign_remote(kRemoteA, TeamId::A);
    REQUIRE(service.start_match(TeamId::A).has_value());

    bool probed = false;
    store.probe = [&](const CommittedEvent& event) {
        if (!std::holds_alternative<domain::PointAwarded>(event.payload)) {
            return;
        }
        probed = true;
        // At append time the state has not been advanced yet and no ACK has
        // been produced: the record's revision is one ahead of live state.
        CHECK(event.state_revision == service.state().revision + 1);
        CHECK(service.state().current_game.raw_points_a == 0);
    };

    service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
    CHECK(probed);
    const auto acks = service.drain_acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].status == protocol::AckStatus::Accepted);
    CHECK(acks[0].state_revision == service.state().revision);
}

TEST_CASE("opposing press inside the window raises a conflict, organizer resolves") {
    Fixture f;
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
    f.clock.advance(100);  // still inside the 250 ms window
    f.service.handle_point_intent(intent(kRemoteB, TeamId::B, 1));

    // Both presses get a terminal conflict rejection; nothing was scored.
    const auto acks = f.acks();
    REQUIRE(acks.size() == 2);
    CHECK(acks[0].status == protocol::AckStatus::RejectedConflict);
    CHECK(acks[1].status == protocol::AckStatus::RejectedConflict);
    CHECK(f.points_a() == 0);
    CHECK(f.points_b() == 0);
    REQUIRE(f.service.conflict_pending());
    CHECK(f.service.counters().conflicts == 1);

    SECTION("organizer awards team A") {
        const auto resolved = f.service.resolve_conflict(TeamId::A);
        REQUIRE(resolved.has_value());
        CHECK(resolved.value().has_value());
        CHECK(f.points_a() == 1);
        CHECK_FALSE(f.service.conflict_pending());
        // Resolution is journaled as an organizer-sourced point.
        REQUIRE(f.store.point_count() == 1);
        CHECK(f.store.events.back().source == InputSource::TouchscreenAdmin);
    }

    SECTION("organizer cancels the rally point") {
        const auto resolved = f.service.resolve_conflict(std::nullopt);
        REQUIRE(resolved.has_value());
        CHECK_FALSE(resolved.value().has_value());
        CHECK(f.points_a() == 0);
        CHECK(f.points_b() == 0);
        CHECK_FALSE(f.service.conflict_pending());
    }

    SECTION("resolving without a conflict is an error") {
        REQUIRE(f.service.resolve_conflict(TeamId::A).has_value());
        const auto again = f.service.resolve_conflict(TeamId::A);
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error() == ServiceError::NoConflictPending);
    }
}

TEST_CASE("lost-ACK retry of a conflicted press is re-ACKed, window not reopened") {
    Fixture f;
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
    f.service.handle_point_intent(intent(kRemoteB, TeamId::B, 1));
    f.acks();
    REQUIRE(f.service.resolve_conflict(std::nullopt).has_value());

    // Remote A never saw its RejectedConflict and retries the same intent.
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
    const auto acks = f.acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].status == protocol::AckStatus::RejectedConflict);

    f.expire_window();
    CHECK(f.acks().empty());
    CHECK(f.points_a() == 0);
}

TEST_CASE("press while a conflict is unresolved joins the conflict") {
    Fixture f;
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
    f.service.handle_point_intent(intent(kRemoteB, TeamId::B, 1));
    f.acks();

    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 2));
    const auto acks = f.acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].status == protocol::AckStatus::RejectedConflict);
    CHECK(f.points_a() == 0);
}

TEST_CASE("no opposing press: pending point auto-commits at expiry, not before") {
    Fixture f;
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));

    f.clock.advance(249);
    f.service.tick();
    CHECK(f.points_a() == 0);
    CHECK(f.acks().empty());

    f.clock.advance(2);
    f.service.tick();
    CHECK(f.points_a() == 1);
    REQUIRE(f.acks().size() == 1);
}

TEST_CASE("second distinct same-team press commits the parked one immediately") {
    Fixture f;
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
    f.clock.advance(50);
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 2));

    // First press committed on the spot; second is now parked.
    auto acks = f.acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].status == protocol::AckStatus::Accepted);
    CHECK(acks[0].identity.sequence == 1);
    CHECK(f.points_a() == 1);

    f.expire_window();
    acks = f.acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].identity.sequence == 2);
    CHECK(f.points_a() == 2);
}

TEST_CASE("wired and wireless inputs converge on the same path") {
    Fixture f;

    SECTION("backup button participates in the conflict guard") {
        const auto result = f.service.award_point_local(TeamId::A, InputSource::PhysicalBackupButton);
        CHECK(result.outcome == LocalPointOutcome::PendingConflictWindow);

        // Remote press for the other team inside the window -> conflict.
        f.service.handle_point_intent(intent(kRemoteB, TeamId::B, 1));
        REQUIRE(f.service.conflict_pending());
        const auto acks = f.acks();
        REQUIRE(acks.size() == 1);  // only the remote press has an identity to ACK
        CHECK(acks[0].status == protocol::AckStatus::RejectedConflict);
    }

    SECTION("backup button point commits at window expiry and is journaled") {
        REQUIRE(f.service.award_point_local(TeamId::A, InputSource::PhysicalBackupButton).outcome ==
                LocalPointOutcome::PendingConflictWindow);
        f.expire_window();
        CHECK(f.points_a() == 1);
        REQUIRE(f.store.point_count() == 1);
        CHECK(f.store.events.back().source == InputSource::PhysicalBackupButton);
        CHECK_FALSE(f.store.events.back().intent.has_value());
    }

    SECTION("organizer touch bypasses the guard and commits synchronously") {
        const auto result = f.service.award_point_local(TeamId::A, InputSource::TouchscreenAdmin);
        CHECK(result.outcome == LocalPointOutcome::Committed);
        CHECK(f.points_a() == 1);
    }

    SECTION("local press is validated like any other") {
        REQUIRE(f.service.pause_match().has_value());
        const auto result = f.service.award_point_local(TeamId::A, InputSource::PhysicalBackupButton);
        CHECK(result.outcome == LocalPointOutcome::Rejected);
        REQUIRE(result.error.has_value());
        CHECK(*result.error == ServiceError::MatchPaused);
    }
}

TEST_CASE("window disabled: first press wins and commits synchronously") {
    Fixture f(0);
    f.service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
    const auto acks = f.acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].status == protocol::AckStatus::Accepted);
    CHECK(f.points_a() == 1);
}

TEST_CASE("lifecycle commands are journaled and storage failure blocks them") {
    Fixture f;
    REQUIRE(f.service.award_point_local(TeamId::A, InputSource::Simulator).outcome ==
            LocalPointOutcome::Committed);

    const auto undo = f.service.undo_last_scoring_action();
    REQUIRE(undo.has_value());
    CHECK(f.points_a() == 0);
    CHECK(std::holds_alternative<domain::ScoringActionUndone>(f.store.events.back().payload));

    f.store.fail = true;
    const auto paused = f.service.pause_match();
    REQUIRE_FALSE(paused.has_value());
    CHECK(paused.error() == ServiceError::StorageFailure);
    CHECK(f.service.state().lifecycle == domain::MatchLifecycle::Active);
}

TEST_CASE("recovery constructor rebuilds state and dedup watermarks") {
    FakeClock clock;
    FakeEventStore store;
    {
        CourtService service(CourtServiceConfig{kCourt, 0},
                             domain::preset_standard_advantage(), store, clock);
        service.assign_remote(kRemoteA, TeamId::A);
        REQUIRE(service.start_match(TeamId::A).has_value());
        service.handle_point_intent(intent(kRemoteA, TeamId::A, 1));
        service.handle_point_intent(intent(kRemoteA, TeamId::A, 2));
        REQUIRE(service.drain_acks().size() == 2);
    }

    // "Reboot": rebuild from the journaled events.
    FakeEventStore store2;
    CourtService rebooted(CourtServiceConfig{kCourt, 0},
                          domain::preset_standard_advantage(), store.events, store2, clock);
    rebooted.assign_remote(kRemoteA, TeamId::A);
    CHECK(rebooted.state().current_game.raw_points_a == 2);
    CHECK(rebooted.state().lifecycle == domain::MatchLifecycle::Active);

    // A retried pre-reboot intent must classify as Duplicate, not score again.
    rebooted.handle_point_intent(intent(kRemoteA, TeamId::A, 2));
    const auto acks = rebooted.drain_acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].status == protocol::AckStatus::DuplicateAccepted);
    CHECK(rebooted.state().current_game.raw_points_a == 2);
}
