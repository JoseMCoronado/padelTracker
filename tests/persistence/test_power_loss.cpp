// Spec section 13.5: power-loss fault-injection matrix. The full stack
// (CourtService + JournalEventStore + InMemoryFileBackend) is interrupted at
// every stage of the accept pipeline; after each "reboot" (recover + rebuild)
// the invariants hold: no accepted point lost, no retried point applied twice.
#include <catch2/catch_test_macros.hpp>

#include "../application/fakes.hpp"
#include "padel/application/court_service.hpp"
#include "padel/persistence/file_backend.hpp"
#include "padel/persistence/journal.hpp"

using namespace padel;
using namespace padel::application;
using namespace padel::application::testing;
using namespace padel::persistence;

namespace {

constexpr CourtId kCourt = 3;
constexpr RemoteId kRemote = 0xA1;

protocol::PointIntentPacket press(std::uint32_t seq) {
    protocol::PointIntentPacket packet{};
    packet.court_id = kCourt;
    packet.identity = protocol::IntentIdentity{kRemote, 1, seq};
    packet.team = TeamId::A;
    return packet;
}

std::size_t journaled_points(const InMemoryFileBackend& backend) {
    std::size_t count = 0;
    for (const CommittedEvent& event : recover(backend.read_all()).events) {
        if (std::holds_alternative<domain::PointAwarded>(event.payload)) {
            ++count;
        }
    }
    return count;
}

struct Court {
    FakeClock clock{};
    InMemoryFileBackend& backend;
    JournalEventStore store;
    CourtService service;

    // Boots the court from whatever survived in the backend.
    explicit Court(InMemoryFileBackend& b)
        : backend(b),
          store(b, recover(b.read_all()).valid_bytes),
          service(CourtServiceConfig{kCourt, 0},  // window 0: focus on storage
                  domain::preset_standard_advantage(),
                  recover(b.read_all()).events, store, clock) {
        backend.truncate(recover(backend.read_all()).valid_bytes);
        service.assign_remote(kRemote, TeamId::A);
    }
};

}  // namespace

TEST_CASE("power loss before the append: press simply never happened") {
    InMemoryFileBackend backend;
    {
        Court court(backend);
        REQUIRE(court.service.start_match(TeamId::A).has_value());
        // Power dies before the press reaches the service: nothing to do.
        backend.simulate_power_loss();
    }

    Court rebooted(backend);
    CHECK(rebooted.service.state().lifecycle == domain::MatchLifecycle::Active);
    CHECK(journaled_points(backend) == 0);

    // The remote retries: applied exactly once.
    rebooted.service.handle_point_intent(press(1));
    REQUIRE(rebooted.service.drain_acks().size() == 1);
    CHECK(journaled_points(backend) == 1);
}

TEST_CASE("power loss during the append: torn record, retry applies exactly once") {
    InMemoryFileBackend backend;
    {
        Court court(backend);
        REQUIRE(court.service.start_match(TeamId::A).has_value());
        backend.tear_next_append(11);  // power dies mid-write of the point record
        court.service.handle_point_intent(press(1));
        const auto acks = court.service.drain_acks();
        REQUIRE(acks.size() == 1);
        CHECK(acks[0].status == protocol::AckStatus::ErrorStorage);
        CHECK(court.service.state().current_game.raw_points_a == 0);
        backend.simulate_power_loss();
    }

    // The torn tail (if any survived) is dropped at recovery; state has the
    // match started but no point.
    Court rebooted(backend);
    CHECK(rebooted.service.state().current_game.raw_points_a == 0);

    // The remote never got a terminal Accepted, so it retries: exactly once.
    rebooted.service.handle_point_intent(press(1));
    const auto acks = rebooted.service.drain_acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].status == protocol::AckStatus::Accepted);
    CHECK(rebooted.service.state().current_game.raw_points_a == 1);
    CHECK(journaled_points(backend) == 1);
}

TEST_CASE("power loss after durable append but before the ACK reaches the remote") {
    InMemoryFileBackend backend;
    {
        Court court(backend);
        REQUIRE(court.service.start_match(TeamId::A).has_value());
        court.service.handle_point_intent(press(1));
        // The Accepted ACK was produced but the court dies before the radio
        // sends it (we simply never drain the outbox).
        backend.simulate_power_loss();
    }

    Court rebooted(backend);
    // The accepted point survived (it was durable before the ACK existed).
    CHECK(rebooted.service.state().current_game.raw_points_a == 1);
    CHECK(journaled_points(backend) == 1);

    // The unACKed remote retries: duplicate, not a second point.
    rebooted.service.handle_point_intent(press(1));
    const auto acks = rebooted.service.drain_acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].status == protocol::AckStatus::DuplicateAccepted);
    CHECK(rebooted.service.state().current_game.raw_points_a == 1);
    CHECK(journaled_points(backend) == 1);
}

TEST_CASE("power loss right after the ACK: retry still classified duplicate") {
    InMemoryFileBackend backend;
    {
        Court court(backend);
        REQUIRE(court.service.start_match(TeamId::A).has_value());
        court.service.handle_point_intent(press(1));
        const auto acks = court.service.drain_acks();  // ACK went out
        REQUIRE(acks.size() == 1);
        REQUIRE(acks[0].status == protocol::AckStatus::Accepted);
        backend.simulate_power_loss();
    }

    Court rebooted(backend);
    CHECK(rebooted.service.state().current_game.raw_points_a == 1);

    // Say the ACK was lost in the air anyway; the retry is a duplicate.
    rebooted.service.handle_point_intent(press(1));
    const auto acks = rebooted.service.drain_acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].status == protocol::AckStatus::DuplicateAccepted);
    CHECK(journaled_points(backend) == 1);
}

TEST_CASE("dedup watermark is rebuilt from the journal across multiple points") {
    InMemoryFileBackend backend;
    {
        Court court(backend);
        REQUIRE(court.service.start_match(TeamId::A).has_value());
        for (std::uint32_t seq = 1; seq <= 3; ++seq) {
            court.service.handle_point_intent(press(seq));
        }
        REQUIRE(court.service.drain_acks().size() == 3);
    }

    Court rebooted(backend);
    CHECK(rebooted.service.state().current_game.raw_points_a == 3);

    // Retries of all pre-reboot intents are duplicates; a new one is fresh.
    for (std::uint32_t seq = 1; seq <= 3; ++seq) {
        rebooted.service.handle_point_intent(press(seq));
    }
    rebooted.service.handle_point_intent(press(4));
    const auto acks = rebooted.service.drain_acks();
    REQUIRE(acks.size() == 4);
    CHECK(acks[0].status == protocol::AckStatus::DuplicateAccepted);
    CHECK(acks[1].status == protocol::AckStatus::DuplicateAccepted);
    CHECK(acks[2].status == protocol::AckStatus::DuplicateAccepted);
    CHECK(acks[3].status == protocol::AckStatus::Accepted);
    CHECK(journaled_points(backend) == 4);
}

TEST_CASE("undo survives reboot") {
    InMemoryFileBackend backend;
    {
        Court court(backend);
        REQUIRE(court.service.start_match(TeamId::A).has_value());
        court.service.handle_point_intent(press(1));
        court.service.handle_point_intent(press(2));
        REQUIRE(court.service.undo_last_scoring_action().has_value());
        REQUIRE(court.service.state().current_game.raw_points_a == 1);
    }

    Court rebooted(backend);
    CHECK(rebooted.service.state().current_game.raw_points_a == 1);
    // created, started, two points, undo: revision counts all five events
    CHECK(rebooted.service.state().revision == 5);
}
