#include "padel/protocol/packets.hpp"

#include "padel/protocol/crc16.hpp"

namespace padel::protocol {
namespace {

// Explicit little-endian byte access; raw struct memory never crosses the
// radio (spec section 10.3).

class Writer {
public:
    explicit Writer(std::uint8_t* out) : out_(out) {}

    void u8(std::uint8_t v) { out_[pos_++] = v; }
    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v & 0xFF));
        u8(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v) {
        u16(static_cast<std::uint16_t>(v & 0xFFFF));
        u16(static_cast<std::uint16_t>(v >> 16));
    }
    void u64(std::uint64_t v) {
        u32(static_cast<std::uint32_t>(v & 0xFFFFFFFF));
        u32(static_cast<std::uint32_t>(v >> 32));
    }
    std::size_t position() const { return pos_; }

private:
    std::uint8_t* out_;
    std::size_t pos_ = 0;
};

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t offset) : data_(data), pos_(offset) {}

    std::uint8_t u8() { return data_[pos_++]; }
    std::uint16_t u16() {
        const std::uint16_t lo = u8();
        const std::uint16_t hi = u8();
        return static_cast<std::uint16_t>(lo | (hi << 8));
    }
    std::uint32_t u32() {
        const std::uint32_t lo = u16();
        const std::uint32_t hi = u16();
        return lo | (hi << 16);
    }
    std::uint64_t u64() {
        const std::uint64_t lo = u32();
        const std::uint64_t hi = u32();
        return lo | (hi << 32);
    }

private:
    const std::uint8_t* data_;
    std::size_t pos_;
};

void write_header(Writer& writer, MessageType type) {
    writer.u8(kMagic0);
    writer.u8(kMagic1);
    writer.u8(kProtocolVersion);
    writer.u8(static_cast<std::uint8_t>(type));
}

void append_crc(std::uint8_t* buffer, std::size_t payload_length) {
    const std::uint16_t crc = crc16_ccitt(buffer, payload_length);
    buffer[payload_length] = static_cast<std::uint8_t>(crc & 0xFF);
    buffer[payload_length + 1] = static_cast<std::uint8_t>(crc >> 8);
}

// Validates framing shared by all messages. Returns an error, or the message
// type if the frame is sound. expected_size == 0 means "any type".
Result<MessageType, ProtocolError> validate_frame(const std::uint8_t* data, std::size_t length,
                                                  MessageType expected_type,
                                                  std::size_t expected_size) {
    using R = Result<MessageType, ProtocolError>;
    if (length < 4) {
        return R::err(ProtocolError::Truncated);
    }
    if (data[0] != kMagic0 || data[1] != kMagic1) {
        return R::err(ProtocolError::InvalidMagic);
    }
    if (data[2] != kProtocolVersion) {
        return R::err(ProtocolError::UnsupportedVersion);
    }
    if (data[3] != static_cast<std::uint8_t>(expected_type)) {
        return R::err(ProtocolError::InvalidMessageType);
    }
    if (length < expected_size) {
        return R::err(ProtocolError::Truncated);
    }
    if (length > expected_size) {
        return R::err(ProtocolError::Oversized);
    }
    const std::uint16_t stored =
        static_cast<std::uint16_t>(data[expected_size - 2] |
                                   (static_cast<std::uint16_t>(data[expected_size - 1]) << 8));
    if (stored != crc16_ccitt(data, expected_size - 2)) {
        return R::err(ProtocolError::CrcMismatch);
    }
    return R::ok(expected_type);
}

}  // namespace

std::array<std::uint8_t, kPointIntentSize> serialize(const PointIntentPacket& packet) {
    std::array<std::uint8_t, kPointIntentSize> buffer{};
    Writer writer(buffer.data());
    write_header(writer, MessageType::PointIntent);
    writer.u16(packet.court_id);
    writer.u32(packet.identity.remote_id);
    writer.u32(packet.identity.boot_id);
    writer.u32(packet.identity.sequence);
    writer.u8(static_cast<std::uint8_t>(packet.team));
    writer.u8(static_cast<std::uint8_t>(packet.action));
    writer.u16(packet.button_duration_ms);
    writer.u16(packet.battery_mv);
    writer.u32(packet.monotonic_ms);
    writer.u8(packet.flags);
    append_crc(buffer.data(), writer.position());
    return buffer;
}

std::array<std::uint8_t, kAckSize> serialize(const AckPacket& packet) {
    std::array<std::uint8_t, kAckSize> buffer{};
    Writer writer(buffer.data());
    write_header(writer, MessageType::Ack);
    writer.u16(packet.court_id);
    writer.u32(packet.identity.remote_id);
    writer.u32(packet.identity.boot_id);
    writer.u32(packet.identity.sequence);
    writer.u8(static_cast<std::uint8_t>(packet.status));
    writer.u64(packet.state_revision);
    writer.u8(packet.team_a_display_code);
    writer.u8(packet.team_b_display_code);
    append_crc(buffer.data(), writer.position());
    return buffer;
}

Result<PointIntentPacket, ProtocolError> parse_point_intent(const std::uint8_t* data,
                                                            std::size_t length) {
    using R = Result<PointIntentPacket, ProtocolError>;
    const auto frame = validate_frame(data, length, MessageType::PointIntent, kPointIntentSize);
    if (!frame) {
        return R::err(frame.error());
    }

    Reader reader(data, 4);
    PointIntentPacket packet{};
    packet.court_id = reader.u16();
    packet.identity.remote_id = reader.u32();
    packet.identity.boot_id = reader.u32();
    packet.identity.sequence = reader.u32();

    const std::uint8_t team = reader.u8();
    if (team != static_cast<std::uint8_t>(TeamId::A) &&
        team != static_cast<std::uint8_t>(TeamId::B)) {
        return R::err(ProtocolError::InvalidTeam);
    }
    packet.team = static_cast<TeamId>(team);

    const std::uint8_t action = reader.u8();
    if (action != static_cast<std::uint8_t>(Action::AwardPoint) &&
        action != static_cast<std::uint8_t>(Action::UndoLastPoint)) {
        return R::err(ProtocolError::InvalidAction);
    }
    packet.action = static_cast<Action>(action);

    packet.button_duration_ms = reader.u16();
    packet.battery_mv = reader.u16();
    packet.monotonic_ms = reader.u32();
    packet.flags = reader.u8();
    return R::ok(packet);
}

Result<AckPacket, ProtocolError> parse_ack(const std::uint8_t* data, std::size_t length) {
    using R = Result<AckPacket, ProtocolError>;
    const auto frame = validate_frame(data, length, MessageType::Ack, kAckSize);
    if (!frame) {
        return R::err(frame.error());
    }

    Reader reader(data, 4);
    AckPacket packet{};
    packet.court_id = reader.u16();
    packet.identity.remote_id = reader.u32();
    packet.identity.boot_id = reader.u32();
    packet.identity.sequence = reader.u32();

    const std::uint8_t status = reader.u8();
    if (status < static_cast<std::uint8_t>(AckStatus::Accepted) ||
        status > static_cast<std::uint8_t>(AckStatus::RejectedNothingToUndo)) {
        return R::err(ProtocolError::InvalidAckStatus);
    }
    packet.status = static_cast<AckStatus>(status);

    packet.state_revision = reader.u64();
    packet.team_a_display_code = reader.u8();
    packet.team_b_display_code = reader.u8();
    return R::ok(packet);
}

std::array<std::uint8_t, kPairRequestSize> serialize(const PairRequestPacket& packet) {
    std::array<std::uint8_t, kPairRequestSize> buffer{};
    Writer writer(buffer.data());
    write_header(writer, MessageType::PairRequest);
    writer.u32(packet.remote_id);
    writer.u32(packet.boot_id);
    writer.u8(packet.fw_version);
    writer.u16(packet.battery_mv);
    append_crc(buffer.data(), writer.position());
    return buffer;
}

std::array<std::uint8_t, kPairAssignSize> serialize(const PairAssignPacket& packet) {
    std::array<std::uint8_t, kPairAssignSize> buffer{};
    Writer writer(buffer.data());
    write_header(writer, MessageType::PairAssign);
    writer.u16(packet.court_id);
    writer.u32(packet.remote_id);
    writer.u8(static_cast<std::uint8_t>(packet.team));
    writer.u8(packet.channel);
    append_crc(buffer.data(), writer.position());
    return buffer;
}

Result<PairRequestPacket, ProtocolError> parse_pair_request(const std::uint8_t* data,
                                                            std::size_t length) {
    using R = Result<PairRequestPacket, ProtocolError>;
    const auto frame = validate_frame(data, length, MessageType::PairRequest, kPairRequestSize);
    if (!frame) {
        return R::err(frame.error());
    }
    Reader reader(data, 4);
    PairRequestPacket packet{};
    packet.remote_id = reader.u32();
    packet.boot_id = reader.u32();
    packet.fw_version = reader.u8();
    packet.battery_mv = reader.u16();
    return R::ok(packet);
}

Result<PairAssignPacket, ProtocolError> parse_pair_assign(const std::uint8_t* data,
                                                          std::size_t length) {
    using R = Result<PairAssignPacket, ProtocolError>;
    const auto frame = validate_frame(data, length, MessageType::PairAssign, kPairAssignSize);
    if (!frame) {
        return R::err(frame.error());
    }
    Reader reader(data, 4);
    PairAssignPacket packet{};
    packet.court_id = reader.u16();
    packet.remote_id = reader.u32();
    const std::uint8_t team = reader.u8();
    if (team != static_cast<std::uint8_t>(TeamId::A) &&
        team != static_cast<std::uint8_t>(TeamId::B)) {
        return R::err(ProtocolError::InvalidTeam);
    }
    packet.team = static_cast<TeamId>(team);
    packet.channel = reader.u8();
    return R::ok(packet);
}

Result<MessageType, ProtocolError> peek_message_type(const std::uint8_t* data,
                                                     std::size_t length) {
    using R = Result<MessageType, ProtocolError>;
    if (length < 4) {
        return R::err(ProtocolError::Truncated);
    }
    if (data[0] != kMagic0 || data[1] != kMagic1) {
        return R::err(ProtocolError::InvalidMagic);
    }
    if (data[2] != kProtocolVersion) {
        return R::err(ProtocolError::UnsupportedVersion);
    }
    switch (data[3]) {
        case static_cast<std::uint8_t>(MessageType::PointIntent):
            return R::ok(MessageType::PointIntent);
        case static_cast<std::uint8_t>(MessageType::Ack):
            return R::ok(MessageType::Ack);
        case static_cast<std::uint8_t>(MessageType::PairRequest):
            return R::ok(MessageType::PairRequest);
        case static_cast<std::uint8_t>(MessageType::PairAssign):
            return R::ok(MessageType::PairAssign);
        default:
            return R::err(ProtocolError::InvalidMessageType);
    }
}

}  // namespace padel::protocol
