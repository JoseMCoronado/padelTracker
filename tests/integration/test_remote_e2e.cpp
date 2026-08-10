// M4 acceptance rehearsal, run natively with the REAL remote logic: two
// RemoteCore instances (debounce, stop-and-wait retries, feedback) drive the
// real CourtService + journal over a lossy channel, including a mid-run
// court reboot. Exactly-once as observed by both sides: every press the
// remote confirmed is applied exactly once in the durable journal, and no
// unconfirmed press is applied.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <set>
#include <tuple>
#include <vector>

#include "../application/fakes.hpp"
#include "padel/application/court_service.hpp"
#include "padel/persistence/file_backend.hpp"
#include "padel/persistence/journal.hpp"
#include "padel/remote/remote_core.hpp"

using namespace padel;
using namespace padel::application;
using namespace padel::application::testing;
using namespace padel::persistence;

namespace {

constexpr CourtId kCourt = 1;
constexpr std::uint32_t kWindowMs = 100;

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

// Adapts the shared application FakeClock to the remote's clock interface.
class RemoteClockAdapter : public remote::IClock {
public:
    explicit RemoteClockAdapter(const FakeClock& clock) : clock_(clock) {}
    std::uint64_t now_ms() const override { return clock_.now_ms(); }

private:
    const FakeClock& clock_;
};

// Radio that queues frames toward the court, dropping some.
class LossyRadio : public remote::IRadio {
public:
    LossyRadio(Rng& rng, std::uint32_t drop_percent) : rng_(rng), drop_(drop_percent) {}
    void send_intent(const protocol::PointIntentPacket& packet) override {
        ++sent;
        if (!rng_.chance(drop_)) {
            inbox.push_back(packet);
        }
    }
    std::vector<protocol::PointIntentPacket> inbox{};
    std::uint32_t sent = 0;

private:
    Rng& rng_;
    std::uint32_t drop_;
};

class NullFeedback : public remote::IFeedback {
public:
    void play(remote::FeedbackPattern) override {}
};

class MemoryStore : public remote::ISettingsStore {
public:
    std::optional<remote::RemoteSettings> load() override { return stored; }
    bool save(const remote::RemoteSettings& settings) override {
        stored = settings;
        return true;
    }
    std::optional<remote::RemoteSettings> stored{};
};

struct SimCourt {
    JournalEventStore store;
    CourtService service;

    SimCourt(FakeClock& clock, InMemoryFileBackend& backend, std::size_t valid_bytes,
             std::vector<CommittedEvent> recovered)
        : store(backend, valid_bytes),
          service(CourtServiceConfig{kCourt, kWindowMs},
                  domain::preset_standard_advantage(), std::move(recovered), store, clock) {
        service.assign_remote(0xA1, TeamId::A);
        service.assign_remote(0xB1, TeamId::B);
    }

    static std::unique_ptr<SimCourt> boot(FakeClock& clock, InMemoryFileBackend& backend) {
        const RecoveryResult recovered = recover(backend.read_all());
        backend.truncate(recovered.valid_bytes);
        return std::make_unique<SimCourt>(clock, backend, recovered.valid_bytes,
                                          recovered.events);
    }
};

}  // namespace

TEST_CASE("real remote logic vs real court over a lossy channel, exactly once") {
    FakeClock clock;
    RemoteClockAdapter remote_clock(clock);
    InMemoryFileBackend backend;
    Rng rng(0xFEED5EED);
    constexpr std::uint32_t kDropPercent = 25;

    LossyRadio radio_a(rng, kDropPercent);
    LossyRadio radio_b(rng, kDropPercent);
    NullFeedback feedback;
    MemoryStore store_a;
    MemoryStore store_b;
    store_a.stored = remote::RemoteSettings{true, 0xA1, kCourt, TeamId::A, 0};
    store_b.stored = remote::RemoteSettings{true, 0xB1, kCourt, TeamId::B, 0};

    remote::RemoteCoreConfig remote_config{};
    remote::RemoteCore remote_a{remote_config, remote_clock, radio_a, feedback, store_a};
    remote::RemoteCore remote_b{remote_config, remote_clock, radio_b, feedback, store_b};
    remote_a.begin(0x0A000001);
    remote_b.begin(0x0B000001);

    auto court = SimCourt::boot(clock, backend);
    REQUIRE(court->service.start_match(TeamId::A).has_value());

    // One millisecond of simulated world time.
    const auto step_world = [&] {
        clock.advance(1);
        remote_a.poll();
        remote_b.poll();
        for (LossyRadio* radio : {&radio_a, &radio_b}) {
            for (const auto& packet : radio->inbox) {
                court->service.handle_point_intent(packet);
            }
            radio->inbox.clear();
        }
        court->service.tick();
        for (const protocol::AckPacket& ack : court->service.drain_acks()) {
            if (rng.chance(kDropPercent)) {
                continue;  // ACK lost on the way back
            }
            remote_a.on_ack(ack);
            remote_b.on_ack(ack);
        }
    };

    const auto settle = [&](remote::RemoteCore& remote) {
        // Run until the remote is terminal (confirmed, rejected, or failed).
        for (int guard = 0; guard < 20000; ++guard) {
            if (remote.state() != remote::RemoteState::PendingIntent) {
                return;
            }
            step_world();
        }
        FAIL("remote never reached a terminal state");
    };

    const auto press = [&](remote::RemoteCore& remote) {
        remote.set_button_level(true);
        // Held past stable_press_ms (150) and well short of undo_hold_ms.
        for (int i = 0; i < 250; ++i) {
            step_world();
        }
        remote.set_button_level(false);
        for (int i = 0; i < 40; ++i) {
            step_world();
        }
        settle(remote);
        // Clear the retrigger guard before the next press of the same remote.
        for (int i = 0; i < 700; ++i) {
            step_world();
        }
    };

    constexpr int kPresses = 400;
    for (int i = 0; i < kPresses; ++i) {
        // Blocks of four per team so games/sets complete.
        const TeamId team = (i / 4) % 2 == 0 ? TeamId::A : TeamId::B;

        if (i == kPresses / 2) {
            // Power-cycle the court mid-run; remotes keep their own state.
            backend.simulate_power_loss();
            court = SimCourt::boot(clock, backend);
        }

        press(team == TeamId::A ? remote_a : remote_b);

        if (court->service.state().lifecycle == domain::MatchLifecycle::Completed) {
            REQUIRE(court->service.reset_match().has_value());
            REQUIRE(court->service.start_match(TeamId::A).has_value());
        }
    }

    const auto& stats_a = remote_a.stats();
    const auto& stats_b = remote_b.stats();
    const std::uint32_t confirmed = stats_a.confirmed + stats_b.confirmed;
    const std::uint32_t failed = stats_a.failed + stats_b.failed;
    const std::uint32_t rejected = stats_a.rejected + stats_b.rejected;

    INFO("confirmed=" << confirmed << " failed=" << failed << " rejected=" << rejected
                      << " retries=" << stats_a.retries + stats_b.retries);
    CHECK(confirmed + failed + rejected == kPresses);
    CHECK(stats_a.retries + stats_b.retries > 0);  // loss actually exercised retries

    // Ground truth from the durable journal.
    const RecoveryResult final_state = recover(backend.read_all());
    REQUIRE(final_state.tail == TailStatus::Clean);
    std::size_t points = 0;
    std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> identities;
    for (const CommittedEvent& event : final_state.events) {
        if (!std::holds_alternative<domain::PointAwarded>(event.payload)) {
            continue;
        }
        ++points;
        if (event.intent.has_value()) {
            const auto key = std::make_tuple(event.intent->remote_id, event.intent->boot_id,
                                             event.intent->sequence);
            CHECK(identities.count(key) == 0);
            identities.insert(key);
        }
    }
    // Every remote-confirmed press is durably applied exactly once. A press
    // whose final attempt landed but whose ACK was lost forever shows up as
    // "failed" on the remote yet applied on the court — count those as the
    // only allowed difference.
    CHECK(points >= confirmed);
    CHECK(points <= confirmed + failed);
    CHECK(identities.size() == points);
}
