#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include "padel/protocol/packets.hpp"

using namespace padel;
using namespace padel::protocol;

namespace {

PointIntentPacket sample_intent() {
    PointIntentPacket packet{};
    packet.court_id = 1;
    packet.identity = IntentIdentity{1001, 0xDEADBEEF, 88};
    packet.team = TeamId::B;
    packet.action = Action::AwardPoint;
    packet.button_duration_ms = 120;
    packet.battery_mv = 3700;
    packet.monotonic_ms = 123456;
    packet.flags = 0;
    return packet;
}

AckPacket sample_ack() {
    AckPacket packet{};
    packet.court_id = 1;
    packet.identity = IntentIdentity{1001, 0xDEADBEEF, 88};
    packet.status = AckStatus::Accepted;
    packet.state_revision = 17;
    packet.team_a_display_code = 2;
    packet.team_b_display_code = 1;
    return packet;
}

// Golden vectors generated independently (Python CRC16/CCITT-FALSE); they pin
// the wire format including endianness across compilers and platforms.
constexpr std::array<std::uint8_t, 31> kGoldenIntent = {
    0x50, 0x53, 0x01, 0x01, 0x01, 0x00, 0xE9, 0x03, 0x00, 0x00, 0xEF,
    0xBE, 0xAD, 0xDE, 0x58, 0x00, 0x00, 0x00, 0x02, 0x01, 0x78, 0x00,
    0x74, 0x0E, 0x40, 0xE2, 0x01, 0x00, 0x00, 0x2A, 0x0D};

constexpr std::array<std::uint8_t, 31> kGoldenAck = {
    0x50, 0x53, 0x01, 0x02, 0x01, 0x00, 0xE9, 0x03, 0x00, 0x00, 0xEF,
    0xBE, 0xAD, 0xDE, 0x58, 0x00, 0x00, 0x00, 0x01, 0x11, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x01, 0x7D, 0x7B};

}  // namespace

TEST_CASE("point intent: serialize matches golden vector", "[packets][golden]") {
    REQUIRE(serialize(sample_intent()) == kGoldenIntent);
}

TEST_CASE("ack: serialize matches golden vector", "[packets][golden]") {
    REQUIRE(serialize(sample_ack()) == kGoldenAck);
}

TEST_CASE("point intent: round trip", "[packets]") {
    const auto bytes = serialize(sample_intent());
    const auto result = parse_point_intent(bytes.data(), bytes.size());
    REQUIRE(result.has_value());

    const PointIntentPacket& packet = result.value();
    REQUIRE(packet.court_id == 1);
    REQUIRE(packet.identity == IntentIdentity{1001, 0xDEADBEEF, 88});
    REQUIRE(packet.team == TeamId::B);
    REQUIRE(packet.action == Action::AwardPoint);
    REQUIRE(packet.button_duration_ms == 120);
    REQUIRE(packet.battery_mv == 3700);
    REQUIRE(packet.monotonic_ms == 123456);
    REQUIRE(packet.flags == 0);
}

TEST_CASE("ack: round trip", "[packets]") {
    const auto bytes = serialize(sample_ack());
    const auto result = parse_ack(bytes.data(), bytes.size());
    REQUIRE(result.has_value());

    const AckPacket& packet = result.value();
    REQUIRE(packet.identity == IntentIdentity{1001, 0xDEADBEEF, 88});
    REQUIRE(packet.status == AckStatus::Accepted);
    REQUIRE(packet.state_revision == 17);
    REQUIRE(packet.team_a_display_code == 2);
    REQUIRE(packet.team_b_display_code == 1);
}

TEST_CASE("round trip: extreme field values", "[packets]") {
    PointIntentPacket packet = sample_intent();
    packet.court_id = 0xFFFF;
    packet.identity = IntentIdentity{0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
    packet.monotonic_ms = 0xFFFFFFFF;
    packet.battery_mv = 0;

    const auto bytes = serialize(packet);
    const auto result = parse_point_intent(bytes.data(), bytes.size());
    REQUIRE(result.has_value());
    REQUIRE(result.value().identity == packet.identity);
    REQUIRE(result.value().court_id == 0xFFFF);

    AckPacket ack = sample_ack();
    ack.state_revision = 0xFFFFFFFFFFFFFFFFull;
    const auto ack_bytes = serialize(ack);
    const auto ack_result = parse_ack(ack_bytes.data(), ack_bytes.size());
    REQUIRE(ack_result.has_value());
    REQUIRE(ack_result.value().state_revision == 0xFFFFFFFFFFFFFFFFull);
}

TEST_CASE("parse: invalid magic", "[packets][malformed]") {
    auto bytes = serialize(sample_intent());
    bytes[0] = 'X';
    const auto result = parse_point_intent(bytes.data(), bytes.size());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == ProtocolError::InvalidMagic);
}

TEST_CASE("parse: unsupported version", "[packets][malformed]") {
    auto bytes = serialize(sample_intent());
    bytes[2] = 99;
    const auto result = parse_point_intent(bytes.data(), bytes.size());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == ProtocolError::UnsupportedVersion);
}

TEST_CASE("parse: wrong message type", "[packets][malformed]") {
    const auto bytes = serialize(sample_ack());
    const auto result = parse_point_intent(bytes.data(), bytes.size());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == ProtocolError::InvalidMessageType);
}

TEST_CASE("parse: truncated packet at every length", "[packets][malformed]") {
    const auto bytes = serialize(sample_intent());
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        const auto result = parse_point_intent(bytes.data(), length);
        REQUIRE_FALSE(result.has_value());
        // Short frames are either Truncated (before header/type checks pass)
        // or Truncated by exact-size check; never a parsed packet.
        REQUIRE(result.error() == ProtocolError::Truncated);
    }
}

TEST_CASE("parse: oversized packet", "[packets][malformed]") {
    const auto bytes = serialize(sample_intent());
    std::vector<std::uint8_t> oversized(bytes.begin(), bytes.end());
    oversized.push_back(0x00);
    const auto result = parse_point_intent(oversized.data(), oversized.size());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == ProtocolError::Oversized);
}

TEST_CASE("parse: CRC mismatch on corrupted payload", "[packets][malformed]") {
    auto bytes = serialize(sample_intent());
    bytes[10] ^= 0xFF;  // corrupt remote_id
    const auto result = parse_point_intent(bytes.data(), bytes.size());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == ProtocolError::CrcMismatch);
}

TEST_CASE("parse: CRC mismatch on corrupted CRC field", "[packets][malformed]") {
    auto bytes = serialize(sample_intent());
    bytes[bytes.size() - 1] ^= 0xFF;
    const auto result = parse_point_intent(bytes.data(), bytes.size());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == ProtocolError::CrcMismatch);
}

TEST_CASE("parse: invalid team enum (CRC recomputed)", "[packets][malformed]") {
    PointIntentPacket packet = sample_intent();
    packet.team = static_cast<TeamId>(7);  // invalid on the wire
    const auto bytes = serialize(packet);
    const auto result = parse_point_intent(bytes.data(), bytes.size());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == ProtocolError::InvalidTeam);
}

TEST_CASE("parse: invalid action enum", "[packets][malformed]") {
    PointIntentPacket packet = sample_intent();
    packet.action = static_cast<Action>(0x42);
    const auto bytes = serialize(packet);
    const auto result = parse_point_intent(bytes.data(), bytes.size());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == ProtocolError::InvalidAction);
}

TEST_CASE("parse: invalid ack status enum", "[packets][malformed]") {
    AckPacket packet = sample_ack();
    packet.status = static_cast<AckStatus>(0);
    auto bytes = serialize(packet);
    auto result = parse_ack(bytes.data(), bytes.size());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == ProtocolError::InvalidAckStatus);

    packet.status = static_cast<AckStatus>(200);
    bytes = serialize(packet);
    result = parse_ack(bytes.data(), bytes.size());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == ProtocolError::InvalidAckStatus);
}

TEST_CASE("peek: routes message types and rejects junk", "[packets]") {
    const auto intent_bytes = serialize(sample_intent());
    const auto ack_bytes = serialize(sample_ack());

    REQUIRE(peek_message_type(intent_bytes.data(), intent_bytes.size()).value() ==
            MessageType::PointIntent);
    REQUIRE(peek_message_type(ack_bytes.data(), ack_bytes.size()).value() == MessageType::Ack);

    const std::uint8_t junk[] = {0x00, 0x01, 0x02, 0x03};
    REQUIRE_FALSE(peek_message_type(junk, sizeof(junk)).has_value());
    REQUIRE_FALSE(peek_message_type(intent_bytes.data(), 2).has_value());
}

TEST_CASE("fuzz-lite: random byte mutations never yield a valid parse with bad CRC",
          "[packets][fuzz]") {
    const auto original = serialize(sample_intent());
    std::uint32_t rng = 0xC0FFEE11;
    auto next = [&rng]() {
        rng = rng * 1664525u + 1013904223u;
        return rng;
    };

    for (int i = 0; i < 2000; ++i) {
        auto bytes = original;
        const std::size_t index = next() % bytes.size();
        const std::uint8_t mask = static_cast<std::uint8_t>(next() & 0xFF);
        if (mask == 0) {
            continue;
        }
        bytes[index] ^= mask;
        const auto result = parse_point_intent(bytes.data(), bytes.size());
        if (result.has_value()) {
            // Only acceptable if the mutation hit a field not covered by
            // validation AND the CRC still matches — impossible for a single
            // flipped byte outside the CRC field itself.
            FAIL("mutated packet parsed successfully at byte " << index);
        }
    }
}
