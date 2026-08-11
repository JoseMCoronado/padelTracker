// Court-side pairing flow tests (spec 10.8 / 14.5).
#include <catch2/catch_test_macros.hpp>

#include "fakes.hpp"
#include "padel/application/pairing.hpp"

using namespace padel;
using namespace padel::application;
using namespace padel::application::testing;

namespace {

class FakeSettings : public ISettings {
public:
    std::vector<StoredAssignment> load_assignments() override { return stored; }
    bool save_assignments(const std::vector<StoredAssignment>& assignments) override {
        stored = assignments;
        ++saves;
        return true;
    }
    std::vector<StoredAssignment> stored{};
    int saves = 0;
};

protocol::PairRequestPacket request(std::uint32_t remote_id) {
    protocol::PairRequestPacket packet{};
    packet.remote_id = remote_id;
    packet.boot_id = 0x1111;
    packet.battery_mv = 3700;
    return packet;
}

struct Fixture {
    FakeClock clock{};
    FakeEventStore store{};
    CourtService court{CourtServiceConfig{1, 0}, domain::preset_standard_golden_point(), store,
                       clock};
    FakeSettings settings{};
    PairingService pairing{PairingService::Config{1, 6, 30'000}, court, settings, clock};
};

}  // namespace

TEST_CASE("pairing happy path: request -> candidate -> confirm -> assigned + persisted") {
    Fixture f;
    f.pairing.begin(TeamId::A);
    REQUIRE(f.pairing.active());
    CHECK(f.pairing.seconds_left() == 30);

    f.pairing.handle_pair_request(request(0xAB12CD34));
    REQUIRE(f.pairing.candidate().has_value());
    CHECK(f.pairing.candidate()->short_id == "CD34");

    const auto assign = f.pairing.confirm();
    REQUIRE(assign.has_value());
    CHECK(assign->remote_id == 0xAB12CD34);
    CHECK(assign->team == TeamId::A);
    CHECK(assign->court_id == 1);
    CHECK(assign->channel == 6);

    CHECK_FALSE(f.pairing.active());
    CHECK(f.court.remote_team(0xAB12CD34) == TeamId::A);
    REQUIRE(f.settings.stored.size() == 1);
    CHECK(f.settings.stored.front().remote_id == 0xAB12CD34);
    CHECK(f.settings.stored.front().team == TeamId::A);
}

TEST_CASE("requests outside a window are ignored") {
    Fixture f;
    f.pairing.handle_pair_request(request(0x1));
    CHECK_FALSE(f.pairing.candidate().has_value());
    CHECK_FALSE(f.pairing.confirm().has_value());
}

TEST_CASE("window expires via tick") {
    Fixture f;
    f.pairing.begin(TeamId::B);
    f.clock.advance(30'001);
    f.pairing.tick();
    CHECK_FALSE(f.pairing.active());
}

TEST_CASE("a remote assigned to the other team cannot be silently replaced") {
    Fixture f;
    f.pairing.begin(TeamId::A);
    f.pairing.handle_pair_request(request(0xAAAA));
    f.pairing.confirm();

    // Now someone tries to pair the same remote onto Team B.
    f.pairing.begin(TeamId::B);
    f.pairing.handle_pair_request(request(0xAAAA));
    CHECK_FALSE(f.pairing.candidate().has_value());

    // After explicit unassignment it works.
    f.pairing.unassign(0xAAAA);
    CHECK(f.court.remote_team(0xAAAA) == std::nullopt);
    f.pairing.handle_pair_request(request(0xAAAA));
    REQUIRE(f.pairing.candidate().has_value());
    const auto assign = f.pairing.confirm();
    REQUIRE(assign.has_value());
    CHECK(assign->team == TeamId::B);
    CHECK(f.court.remote_team(0xAAAA) == TeamId::B);
}

TEST_CASE("re-pairing to the same team refreshes without duplicates") {
    Fixture f;
    f.pairing.begin(TeamId::A);
    f.pairing.handle_pair_request(request(0xBBBB));
    f.pairing.confirm();

    f.pairing.begin(TeamId::A);
    f.pairing.handle_pair_request(request(0xBBBB));
    REQUIRE(f.pairing.candidate().has_value());
    f.pairing.confirm();
    CHECK(f.settings.stored.size() == 1);
}

TEST_CASE("persisted assignments are restored at boot") {
    Fixture f;
    f.settings.stored = {{0x1001, TeamId::A}, {0x2002, TeamId::B}};
    f.pairing.load_assignments();
    CHECK(f.court.remote_team(0x1001) == TeamId::A);
    CHECK(f.court.remote_team(0x2002) == TeamId::B);
}

TEST_CASE("unassigning a team frees the slot for a different remote") {
    Fixture f;
    f.pairing.begin(TeamId::A);
    f.pairing.handle_pair_request(request(0x1111));
    f.pairing.confirm();
    REQUIRE(f.court.remote_info(TeamId::A).has_value());

    const std::uint32_t old_remote = f.court.remote_info(TeamId::A)->remote_id;
    f.pairing.unassign(old_remote);
    CHECK_FALSE(f.court.remote_info(TeamId::A).has_value());
    CHECK(f.settings.stored.empty());

    // A different clicker takes the freed slot, and the old one stays out.
    f.pairing.begin(TeamId::A);
    f.pairing.handle_pair_request(request(0x2222));
    REQUIRE(f.pairing.confirm().has_value());
    CHECK(f.court.remote_team(0x2222) == TeamId::A);
    CHECK(f.court.remote_team(0x1111) == std::nullopt);
    REQUIRE(f.settings.stored.size() == 1);
    CHECK(f.settings.stored.front().remote_id == 0x2222);
}

TEST_CASE("an unassigned remote's points are rejected as unpaired") {
    Fixture f;
    f.pairing.begin(TeamId::A);
    f.pairing.handle_pair_request(request(0x3333));
    f.pairing.confirm();
    REQUIRE(f.court.start_match(TeamId::A).has_value());

    protocol::PointIntentPacket packet{};
    packet.court_id = 1;
    packet.identity = protocol::IntentIdentity{0x3333, 0x7777, 1};
    packet.team = TeamId::A;
    f.court.handle_point_intent(packet);
    REQUIRE(f.court.drain_acks().size() == 1);

    f.pairing.unassign(0x3333);
    packet.identity.sequence = 2;
    f.court.handle_point_intent(packet);
    const auto acks = f.court.drain_acks();
    REQUIRE(acks.size() == 1);
    CHECK(acks.front().status == protocol::AckStatus::RejectedUnpaired);
    // The clicker needs the court id to trust the rejection enough to wipe
    // its own credentials.
    CHECK(acks.front().court_id == 1);
}

TEST_CASE("newest request wins while the window is open") {
    Fixture f;
    f.pairing.begin(TeamId::A);
    f.pairing.handle_pair_request(request(0x0001));
    f.pairing.handle_pair_request(request(0x0002));
    REQUIRE(f.pairing.candidate().has_value());
    CHECK(f.pairing.candidate()->remote_id == 0x0002);
}
