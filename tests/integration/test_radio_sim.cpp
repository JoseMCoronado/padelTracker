// Spec section 18.5, run natively: a simulated remote drives the full stack
// (CourtService + journal-backed store + in-memory file backend) through a
// lossy channel. 1000 presses with induced intent/ACK loss and duplicate
// deliveries, plus a mid-run reboot, must yield exactly 1000 applied points.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <set>
#include <tuple>

#include "../application/fakes.hpp"
#include "padel/application/court_service.hpp"
#include "padel/persistence/file_backend.hpp"
#include "padel/persistence/journal.hpp"

using namespace padel;
using namespace padel::application;
using namespace padel::application::testing;
using namespace padel::persistence;

namespace {

constexpr CourtId kCourt = 1;
constexpr RemoteId kRemoteA = 0xAA;
constexpr RemoteId kRemoteB = 0xBB;
constexpr std::uint32_t kWindowMs = 100;

// Deterministic xorshift PRNG so failures reproduce.
struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed) {}
    std::uint32_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<std::uint32_t>(state >> 32);
    }
    bool chance(std::uint32_t percent) { return next() % 100 < percent; }
};

struct Channel {
    Rng rng;
    std::uint32_t drop_percent;
    std::uint32_t duplicate_percent;

    // Returns how many copies of a frame actually arrive (0 = lost).
    int deliveries() {
        if (rng.chance(drop_percent)) {
            return 0;
        }
        return rng.chance(duplicate_percent) ? 2 : 1;
    }
};

struct SimCourt {
    FakeClock& clock;
    InMemoryFileBackend& backend;
    JournalEventStore store;
    CourtService service;

    SimCourt(FakeClock& clk, InMemoryFileBackend& b, std::size_t valid_bytes,
             std::vector<CommittedEvent> recovered)
        : clock(clk),
          backend(b),
          store(b, valid_bytes),
          service(CourtServiceConfig{kCourt, kWindowMs},
                  domain::preset_standard_advantage(), std::move(recovered), store, clock) {
        service.assign_remote(kRemoteA, TeamId::A);
        service.assign_remote(kRemoteB, TeamId::B);
    }

    static std::unique_ptr<SimCourt> boot(FakeClock& clk, InMemoryFileBackend& b) {
        const RecoveryResult recovered = recover(b.read_all());
        b.truncate(recovered.valid_bytes);
        return std::make_unique<SimCourt>(clk, b, recovered.valid_bytes, recovered.events);
    }
};

bool terminal_success(protocol::AckStatus status) {
    return status == protocol::AckStatus::Accepted ||
           status == protocol::AckStatus::DuplicateAccepted;
}

}  // namespace

TEST_CASE("1000 lossy presses with a mid-run reboot apply exactly once each") {
    FakeClock clock;
    InMemoryFileBackend backend;
    Channel channel{Rng(0xC0FFEE), /*drop_percent=*/25, /*duplicate_percent=*/10};

    auto court = SimCourt::boot(clock, backend);
    REQUIRE(court->service.start_match(TeamId::A).has_value());

    std::uint32_t seq_a = 0;
    std::uint32_t seq_b = 0;
    std::uint32_t confirmed = 0;
    std::uint32_t total_attempts = 0;

    constexpr int kPresses = 1000;
    for (int press = 0; press < kPresses; ++press) {
        // Blocks of four presses per team so games and sets actually complete.
        const TeamId team = (press / 4) % 2 == 0 ? TeamId::A : TeamId::B;
        protocol::PointIntentPacket packet{};
        packet.court_id = kCourt;
        packet.team = team;
        packet.identity = protocol::IntentIdentity{
            team == TeamId::A ? kRemoteA : kRemoteB, 1,
            team == TeamId::A ? ++seq_a : ++seq_b};

        // Mid-run reboot: power-cycle the court between presses.
        if (press == kPresses / 2) {
            backend.simulate_power_loss();
            court = SimCourt::boot(clock, backend);
        }

        // Stop-and-wait remote: retry the same identity until a terminal ACK
        // arrives (the real remote gives up after 5 tries; the sim persists so
        // every press eventually lands and the exactly-once bound is exact).
        bool acked = false;
        while (!acked) {
            ++total_attempts;
            const int copies = channel.deliveries();
            for (int c = 0; c < copies; ++c) {
                court->service.handle_point_intent(packet);
            }

            // Give the conflict window time to expire, then collect ACKs.
            clock.advance(kWindowMs + 10);
            court->service.tick();
            for (const protocol::AckPacket& ack : court->service.drain_acks()) {
                if (!(ack.identity == packet.identity)) {
                    continue;
                }
                if (channel.deliveries() == 0) {
                    continue;  // ACK lost on the way back
                }
                if (terminal_success(ack.status)) {
                    acked = true;
                } else {
                    FAIL("unexpected terminal status " << static_cast<int>(ack.status));
                }
            }
            clock.advance(350);  // remote backoff before the retry
        }
        ++confirmed;

        // A completed match blocks further points; reset and keep going.
        if (court->service.state().lifecycle == domain::MatchLifecycle::Completed) {
            REQUIRE(court->service.reset_match().has_value());
            REQUIRE(court->service.start_match(TeamId::A).has_value());
        }
    }

    CHECK(confirmed == kPresses);
    CHECK(total_attempts > kPresses);  // loss actually forced retries

    // Ground truth from the durable journal: exactly 1000 points, each
    // intent identity applied at most once.
    const RecoveryResult final_state = recover(backend.read_all());
    REQUIRE(final_state.tail == TailStatus::Clean);
    std::size_t points = 0;
    std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> identities;
    for (const CommittedEvent& event : final_state.events) {
        if (!std::holds_alternative<domain::PointAwarded>(event.payload)) {
            continue;
        }
        ++points;
        REQUIRE(event.intent.has_value());
        const auto key = std::make_tuple(event.intent->remote_id, event.intent->boot_id,
                                         event.intent->sequence);
        CHECK(identities.count(key) == 0);
        identities.insert(key);
    }
    CHECK(points == kPresses);
    CHECK(identities.size() == kPresses);
}
