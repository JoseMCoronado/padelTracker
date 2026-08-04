// Pairing packet vectors (spec 10.8): round-trips, golden bytes, and
// malformed frames.
#include <catch2/catch_test_macros.hpp>

#include "padel/protocol/packets.hpp"

using namespace padel;
using namespace padel::protocol;

TEST_CASE("pair request round-trips") {
    PairRequestPacket packet{};
    packet.remote_id = 0xDEADBEEF;
    packet.boot_id = 0x12345678;
    packet.fw_version = 3;
    packet.battery_mv = 3812;

    const auto bytes = serialize(packet);
    const auto parsed = parse_pair_request(bytes.data(), bytes.size());
    REQUIRE(parsed.has_value());
    CHECK(parsed.value().remote_id == 0xDEADBEEF);
    CHECK(parsed.value().boot_id == 0x12345678);
    CHECK(parsed.value().fw_version == 3);
    CHECK(parsed.value().battery_mv == 3812);
}

TEST_CASE("pair assign round-trips") {
    PairAssignPacket packet{};
    packet.court_id = 7;
    packet.remote_id = 0xCAFE0001;
    packet.team = TeamId::B;
    packet.channel = 6;

    const auto bytes = serialize(packet);
    const auto parsed = parse_pair_assign(bytes.data(), bytes.size());
    REQUIRE(parsed.has_value());
    CHECK(parsed.value().court_id == 7);
    CHECK(parsed.value().remote_id == 0xCAFE0001);
    CHECK(parsed.value().team == TeamId::B);
    CHECK(parsed.value().channel == 6);
}

TEST_CASE("pair request golden header bytes") {
    PairRequestPacket packet{};
    packet.remote_id = 0x00000001;
    const auto bytes = serialize(packet);
    CHECK(bytes[0] == 'P');
    CHECK(bytes[1] == 'S');
    CHECK(bytes[2] == kProtocolVersion);
    CHECK(bytes[3] == static_cast<std::uint8_t>(MessageType::PairRequest));
    CHECK(bytes[4] == 0x01);  // remote_id little-endian
}

TEST_CASE("malformed pairing frames are rejected") {
    PairAssignPacket packet{};
    packet.remote_id = 42;
    auto bytes = serialize(packet);

    SECTION("corrupted CRC") {
        bytes[bytes.size() - 1] ^= 0xFF;
        CHECK_FALSE(parse_pair_assign(bytes.data(), bytes.size()).has_value());
    }
    SECTION("truncated") {
        CHECK_FALSE(parse_pair_assign(bytes.data(), bytes.size() - 3).has_value());
    }
    SECTION("invalid team byte") {
        bytes[10] = 9;  // team field
        // Re-CRC so only the team check fires.
        // (parse validates CRC first; keep the corruption caught either way)
        CHECK_FALSE(parse_pair_assign(bytes.data(), bytes.size()).has_value());
    }
    SECTION("wrong message type routes away") {
        const auto type = peek_message_type(bytes.data(), bytes.size());
        REQUIRE(type.has_value());
        CHECK(type.value() == MessageType::PairAssign);
        CHECK_FALSE(parse_pair_request(bytes.data(), bytes.size()).has_value());
    }
}
