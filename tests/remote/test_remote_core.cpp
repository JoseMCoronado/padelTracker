// remote_core state machine tests (spec 11.1/11.2/11.3/11.5): debounce,
// stop-and-wait retries, feedback table, sequence persistence.
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

#include "padel/remote/remote_core.hpp"

using namespace padel;
using namespace padel::remote;

namespace {

class FakeClock : public IClock {
public:
    std::uint64_t now_ms() const override { return now_; }
    void advance(std::uint64_t ms) { now_ += ms; }

private:
    std::uint64_t now_ = 1000;
};

class FakeRadio : public IRadio {
public:
    void send_intent(const protocol::PointIntentPacket& packet) override {
        sent.push_back(packet);
    }
    void send_pair_request(const protocol::PairRequestPacket& packet) override {
        pair_requests.push_back(packet);
    }
    std::vector<protocol::PointIntentPacket> sent{};
    std::vector<protocol::PairRequestPacket> pair_requests{};
};

class FakeFeedback : public IFeedback {
public:
    void play(FeedbackPattern pattern) override { played.push_back(pattern); }
    int count(FeedbackPattern pattern) const {
        int n = 0;
        for (const FeedbackPattern p : played) {
            if (p == pattern) ++n;
        }
        return n;
    }
    std::vector<FeedbackPattern> played{};
};

class FakeStore : public ISettingsStore {
public:
    std::optional<RemoteSettings> load() override { return stored; }
    bool save(const RemoteSettings& settings) override {
        stored = settings;
        ++saves;
        return true;
    }
    std::optional<RemoteSettings> stored{};
    int saves = 0;
};

struct Fixture {
    FakeClock clock{};
    FakeRadio radio{};
    FakeFeedback feedback{};
    FakeStore store{};
    RemoteCoreConfig config{};
    RemoteCore core{config, clock, radio, feedback, store};

    Fixture() {
        RemoteSettings settings{};
        settings.paired = true;
        settings.remote_id = 0xA1;
        settings.court_id = 1;
        settings.team = TeamId::A;
        settings.sequence_baseline = 0;
        store.stored = settings;
        core.begin(0xB007'1D01);
    }

    // Advance time in small steps, polling like a firmware loop would.
    void run(std::uint64_t ms) {
        for (std::uint64_t t = 0; t < ms; t += 5) {
            clock.advance(5);
            core.poll();
        }
    }

    // A clean debounced press-and-release.
    void press(std::uint32_t hold_ms = 60) {
        core.set_button_level(true);
        run(hold_ms);
        core.set_button_level(false);
        run(60);
    }

    protocol::AckPacket ack_for(const protocol::PointIntentPacket& intent,
                                protocol::AckStatus status) {
        protocol::AckPacket ack{};
        ack.court_id = intent.court_id;
        ack.identity = intent.identity;
        ack.status = status;
        return ack;
    }
};

}  // namespace

TEST_CASE("switch bounce does not register a press") {
    Fixture f;
    // 10 ms blips, well under the 30 ms stability requirement.
    for (int i = 0; i < 5; ++i) {
        f.core.set_button_level(true);
        f.run(10);
        f.core.set_button_level(false);
        f.run(10);
    }
    CHECK(f.core.stats().presses == 0);
    CHECK(f.radio.sent.empty());
}

TEST_CASE("one stable press = one intent; long hold still one point") {
    Fixture f;
    f.core.set_button_level(true);
    f.run(2000);  // long hold
    f.core.set_button_level(false);
    f.run(60);
    CHECK(f.core.stats().presses == 1);
    REQUIRE(f.core.stats().intents_sent == 1);
    CHECK(f.radio.sent.front().identity.sequence == 1);
    CHECK(f.radio.sent.front().identity.remote_id == 0xA1);
    CHECK(f.radio.sent.front().identity.boot_id == 0xB007'1D01);
    CHECK(f.feedback.count(FeedbackPattern::PressRegistered) == 1);
}

TEST_CASE("double tap inside the retrigger guard is suppressed") {
    Fixture f;
    f.press(60);   // t ~ +120ms
    f.press(60);   // second press well inside the 700 ms guard
    CHECK(f.core.stats().presses == 1);
    CHECK(f.core.stats().presses_suppressed >= 1);
    CHECK(f.core.stats().intents_sent == 1);
}

TEST_CASE("unpaired press gives pairing feedback and sends nothing") {
    Fixture f;
    f.core.clear_pairing();
    f.press();
    CHECK(f.radio.sent.empty());
    CHECK(f.feedback.count(FeedbackPattern::PairingRequired) == 1);
    CHECK(f.core.state() == RemoteState::PairingRequired);
}

TEST_CASE("accepted ACK confirms and frees the remote") {
    Fixture f;
    f.press();
    REQUIRE(f.core.state() == RemoteState::PendingIntent);
    f.core.on_ack(f.ack_for(f.radio.sent.front(), protocol::AckStatus::Accepted));
    CHECK(f.core.state() == RemoteState::Ready);
    CHECK(f.core.stats().confirmed == 1);
    CHECK(f.feedback.count(FeedbackPattern::Accepted) == 1);
}

TEST_CASE("retries reuse the identity and exhaust into CommFailed") {
    Fixture f;
    f.press();
    // No ACK ever: 5 attempts total then failure feedback.
    f.run(10000);
    CHECK(f.radio.sent.size() == 5);
    for (const auto& packet : f.radio.sent) {
        CHECK(packet.identity == f.radio.sent.front().identity);
    }
    CHECK(f.core.stats().failed == 1);
    CHECK(f.core.state() == RemoteState::Ready);
    CHECK(f.feedback.count(FeedbackPattern::CommFailed) == 1);
}

TEST_CASE("DuplicateAccepted after a retry counts as confirmed") {
    Fixture f;
    f.press();
    f.run(600);  // first timeout passed, at least one retry out
    REQUIRE(f.radio.sent.size() >= 2);
    f.core.on_ack(f.ack_for(f.radio.sent.back(), protocol::AckStatus::DuplicateAccepted));
    CHECK(f.core.stats().confirmed == 1);
    CHECK(f.core.state() == RemoteState::Ready);
}

TEST_CASE("foreign or stale ACKs are ignored") {
    Fixture f;
    f.press();
    protocol::AckPacket foreign = f.ack_for(f.radio.sent.front(), protocol::AckStatus::Accepted);
    foreign.identity.sequence += 7;
    f.core.on_ack(foreign);
    CHECK(f.core.state() == RemoteState::PendingIntent);
    CHECK(f.core.stats().confirmed == 0);
}

TEST_CASE("terminal rejections map through the centralized feedback table") {
    CHECK(feedback_for(protocol::AckStatus::Accepted) == FeedbackPattern::Accepted);
    CHECK(feedback_for(protocol::AckStatus::DuplicateAccepted) == FeedbackPattern::Accepted);
    CHECK(feedback_for(protocol::AckStatus::RejectedConflict) ==
          FeedbackPattern::RejectedConflict);
    CHECK(feedback_for(protocol::AckStatus::RejectedPaused) == FeedbackPattern::RejectedOther);
    CHECK(feedback_for(protocol::AckStatus::RejectedNotInMatch) ==
          FeedbackPattern::RejectedOther);
    CHECK(feedback_for(protocol::AckStatus::ErrorStorage) == FeedbackPattern::CommFailed);

    Fixture f;
    f.press();
    f.core.on_ack(f.ack_for(f.radio.sent.front(), protocol::AckStatus::RejectedPaused));
    CHECK(f.feedback.count(FeedbackPattern::RejectedOther) == 1);
    CHECK(f.core.stats().rejected == 1);
}

TEST_CASE("press while an intent is in flight is suppressed (stop-and-wait)") {
    Fixture f;
    f.press();
    REQUIRE(f.core.state() == RemoteState::PendingIntent);
    f.run(800);  // clear the retrigger guard, intent still pending (retrying)
    f.press();
    CHECK(f.core.stats().intents_sent == 1);
    CHECK(f.core.stats().presses_suppressed >= 1);
}

TEST_CASE("sequence baseline is persisted write-ahead and survives reboot") {
    Fixture f;
    f.press();
    f.core.on_ack(f.ack_for(f.radio.sent.front(), protocol::AckStatus::Accepted));

    // The store was updated before the first send: baseline covers the used
    // sequence plus the chunk.
    REQUIRE(f.store.stored.has_value());
    CHECK(f.store.stored->sequence_baseline > f.core.last_sequence());

    // Reboot: a new core over the same store must start beyond every
    // previously used sequence.
    RemoteCore rebooted{f.config, f.clock, f.radio, f.feedback, f.store};
    rebooted.begin(0xB007'1D02);
    f.radio.sent.clear();
    rebooted.set_button_level(true);
    f.run(60);
    // poll() drives the rebooted core's debounce via the fixture clock.
    for (int i = 0; i < 20; ++i) {
        f.clock.advance(5);
        rebooted.poll();
    }
    rebooted.set_button_level(false);
    REQUIRE(!f.radio.sent.empty());
    CHECK(f.radio.sent.front().identity.sequence > f.core.last_sequence());
}

TEST_CASE("long hold while unpaired enters pairing advertise and broadcasts") {
    Fixture f;
    f.core.clear_pairing();
    f.core.set_button_level(true);
    f.run(5100);  // past the 5 s pairing hold
    CHECK(f.core.state() == RemoteState::PairingAdvertise);
    f.core.set_button_level(false);
    f.run(1600);
    // Broadcasts every 500 ms while advertising; no point intents.
    CHECK(f.radio.pair_requests.size() >= 3);
    CHECK(f.radio.pair_requests.front().remote_id == 0xA1);
    CHECK(f.core.stats().intents_sent == 0);
    CHECK(f.radio.sent.empty());
}

TEST_CASE("pair assign addressed to this remote completes pairing") {
    Fixture f;
    f.core.clear_pairing();
    f.core.enter_pairing_mode();
    REQUIRE(f.core.state() == RemoteState::PairingAdvertise);

    protocol::PairAssignPacket assign{};
    assign.court_id = 9;
    assign.remote_id = f.core.settings().remote_id;
    assign.team = TeamId::B;
    assign.channel = 6;
    f.core.on_pair_assign(assign);

    CHECK(f.core.state() == RemoteState::Ready);
    CHECK(f.core.settings().paired);
    CHECK(f.core.settings().court_id == 9);
    CHECK(f.core.settings().team == TeamId::B);
    CHECK(f.feedback.count(FeedbackPattern::PairingSuccess) == 1);
    // Persisted.
    REQUIRE(f.store.stored.has_value());
    CHECK(f.store.stored->paired);
}

TEST_CASE("pair assign for another remote is ignored") {
    Fixture f;
    f.core.clear_pairing();
    f.core.enter_pairing_mode();
    protocol::PairAssignPacket assign{};
    assign.remote_id = 0xDEAD;  // not us
    assign.team = TeamId::A;
    f.core.on_pair_assign(assign);
    CHECK(f.core.state() == RemoteState::PairingAdvertise);
    CHECK_FALSE(f.core.settings().paired);
}

TEST_CASE("pairing advertise times out back to PairingRequired") {
    Fixture f;
    f.core.clear_pairing();
    f.core.enter_pairing_mode();
    f.run(61'000);
    CHECK(f.core.state() == RemoteState::PairingRequired);
}

TEST_CASE("baseline is only rewritten when the chunk is exhausted") {
    Fixture f;
    const int initial_saves = f.store.saves;
    for (int i = 0; i < 10; ++i) {
        f.press(60);
        f.core.on_ack(f.ack_for(f.radio.sent.back(), protocol::AckStatus::Accepted));
        f.run(700);  // clear the guard
    }
    // 10 presses with a 32-chunk: exactly one baseline write.
    CHECK(f.store.saves - initial_saves == 1);
}
