#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "padel/common/ids.hpp"
#include "padel/common/result.hpp"

namespace padel::protocol {

inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::uint8_t kMagic0 = 'P';
inline constexpr std::uint8_t kMagic1 = 'S';

enum class MessageType : std::uint8_t {
    PointIntent = 0x01,
    Ack = 0x02,
    PairRequest = 0x03,
    PairAssign = 0x04,
};

enum class Action : std::uint8_t {
    AwardPoint = 0x01,
    // Take back the sending team's most recent point (ADR-0014). Shares the
    // POINT_INTENT frame so it inherits the sequence identity, deduplication
    // and retry machinery that make the award path exactly-once.
    UndoLastPoint = 0x02,
};

// Terminal statuses a remote can receive (spec section 10.2).
enum class AckStatus : std::uint8_t {
    Accepted = 1,
    DuplicateAccepted = 2,
    RejectedNotInMatch = 3,
    RejectedWrongTeam = 4,
    RejectedUnpaired = 5,
    RejectedPaused = 6,
    RejectedConflict = 7,
    RejectedInvalidPacket = 8,
    ErrorStorage = 9,
    RejectedNothingToUndo = 10,
};

enum class ProtocolError : std::uint8_t {
    Truncated,
    Oversized,
    InvalidMagic,
    UnsupportedVersion,
    InvalidMessageType,
    CrcMismatch,
    InvalidTeam,
    InvalidAction,
    InvalidAckStatus,
};

// Unique intent identity; retries reuse it (spec section 10.3).
struct IntentIdentity {
    std::uint32_t remote_id = 0;
    std::uint32_t boot_id = 0;
    std::uint32_t sequence = 0;

    bool operator==(const IntentIdentity& other) const {
        return remote_id == other.remote_id && boot_id == other.boot_id &&
               sequence == other.sequence;
    }
};

struct PointIntentPacket {
    std::uint16_t court_id = 0;
    IntentIdentity identity{};
    TeamId team{TeamId::A};
    Action action{Action::AwardPoint};
    std::uint16_t button_duration_ms = 0;
    std::uint16_t battery_mv = 0;  // 0 = unknown
    std::uint32_t monotonic_ms = 0;
    std::uint8_t flags = 0;
};

struct AckPacket {
    std::uint16_t court_id = 0;
    IntentIdentity identity{};
    AckStatus status{AckStatus::Accepted};
    std::uint64_t state_revision = 0;
    std::uint8_t team_a_display_code = 0;
    std::uint8_t team_b_display_code = 0;
};

// Broadcast by a remote in pairing mode (spec 10.8 / 14.5). The court shows
// the short device id (low 16 bits of remote_id, hex) for organizer
// confirmation.
struct PairRequestPacket {
    std::uint32_t remote_id = 0;
    std::uint32_t boot_id = 0;
    std::uint8_t fw_version = 0;
    std::uint16_t battery_mv = 0;
};

// Sent by the court after the organizer confirms a pairing candidate.
struct PairAssignPacket {
    CourtId court_id = 0;
    std::uint32_t remote_id = 0;  // target remote
    TeamId team{TeamId::A};
    std::uint8_t channel = 0;     // Wi-Fi channel the court operates on
};

inline constexpr std::size_t kPointIntentSize = 31;
inline constexpr std::size_t kAckSize = 31;
inline constexpr std::size_t kPairRequestSize = 17;
inline constexpr std::size_t kPairAssignSize = 14;

std::array<std::uint8_t, kPointIntentSize> serialize(const PointIntentPacket& packet);
std::array<std::uint8_t, kAckSize> serialize(const AckPacket& packet);
std::array<std::uint8_t, kPairRequestSize> serialize(const PairRequestPacket& packet);
std::array<std::uint8_t, kPairAssignSize> serialize(const PairAssignPacket& packet);

Result<PointIntentPacket, ProtocolError> parse_point_intent(const std::uint8_t* data,
                                                            std::size_t length);
Result<AckPacket, ProtocolError> parse_ack(const std::uint8_t* data, std::size_t length);
Result<PairRequestPacket, ProtocolError> parse_pair_request(const std::uint8_t* data,
                                                            std::size_t length);
Result<PairAssignPacket, ProtocolError> parse_pair_assign(const std::uint8_t* data,
                                                          std::size_t length);

// Cheap classification for routing before full parse (safe on any length).
Result<MessageType, ProtocolError> peek_message_type(const std::uint8_t* data,
                                                     std::size_t length);

}  // namespace padel::protocol
