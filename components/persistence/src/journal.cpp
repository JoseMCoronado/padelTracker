#include "padel/persistence/journal.hpp"

#include <type_traits>
#include <variant>

#include "padel/protocol/crc16.hpp"

namespace padel::persistence {
namespace {

using application::CommittedEvent;
using namespace padel::domain;

enum class EventType : std::uint8_t {
    Created = 1,
    Started = 2,
    PointAwarded_ = 3,
    Undone = 4,
    ServingChanged = 5,
    Paused = 6,
    Resumed = 7,
    FinishedManually = 8,
    Reset = 9,
};

class Writer {
public:
    explicit Writer(std::vector<std::uint8_t>& out) : out_(out) {}

    void u8(std::uint8_t v) { out_.push_back(v); }
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

private:
    std::vector<std::uint8_t>& out_;
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

void write_config(Writer& w, const MatchConfig& config) {
    w.u8(static_cast<std::uint8_t>(config.game_rule));
    w.u8(config.sets_to_win);
    w.u8(static_cast<std::uint8_t>(config.final_set_rule));
    w.u8(config.match_tiebreak_points_to_win);
    w.u8(config.match_tiebreak_win_by_two ? 1 : 0);
    w.u8(config.track_serving_team ? 1 : 0);
    w.u8(config.normal_set.games_to_win);
    w.u8(config.normal_set.win_by_two_games ? 1 : 0);
    w.u8(config.normal_set.tiebreak_enabled ? 1 : 0);
    w.u8(config.normal_set.tiebreak_at_games);
    w.u8(config.normal_set.tiebreak_points_to_win);
    w.u8(config.normal_set.tiebreak_win_by_two ? 1 : 0);
}

MatchConfig read_config(Reader& r) {
    MatchConfig config{};
    config.game_rule = static_cast<GameRule>(r.u8());
    config.sets_to_win = r.u8();
    config.final_set_rule = static_cast<FinalSetRule>(r.u8());
    config.match_tiebreak_points_to_win = r.u8();
    config.match_tiebreak_win_by_two = r.u8() != 0;
    config.track_serving_team = r.u8() != 0;
    config.normal_set.games_to_win = r.u8();
    config.normal_set.win_by_two_games = r.u8() != 0;
    config.normal_set.tiebreak_enabled = r.u8() != 0;
    config.normal_set.tiebreak_at_games = r.u8();
    config.normal_set.tiebreak_points_to_win = r.u8();
    config.normal_set.tiebreak_win_by_two = r.u8() != 0;
    return config;
}

struct PayloadInfo {
    EventType type;
    std::vector<std::uint8_t> bytes;
};

PayloadInfo serialize_payload(const Event& event) {
    PayloadInfo info{EventType::Reset, {}};
    Writer w(info.bytes);
    std::visit(
        [&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, MatchCreated>) {
                info.type = EventType::Created;
                w.u64(e.match_id);
                write_config(w, e.config);
            } else if constexpr (std::is_same_v<T, MatchStarted>) {
                info.type = EventType::Started;
                w.u8(static_cast<std::uint8_t>(e.initial_serving_team));
            } else if constexpr (std::is_same_v<T, PointAwarded>) {
                info.type = EventType::PointAwarded_;
                w.u8(static_cast<std::uint8_t>(e.team));
                w.u8(static_cast<std::uint8_t>(e.source));
            } else if constexpr (std::is_same_v<T, ScoringActionUndone>) {
                info.type = EventType::Undone;
                w.u64(e.undone_event_id);
            } else if constexpr (std::is_same_v<T, ServingTeamChanged>) {
                info.type = EventType::ServingChanged;
                w.u8(static_cast<std::uint8_t>(e.team));
            } else if constexpr (std::is_same_v<T, MatchPaused>) {
                info.type = EventType::Paused;
            } else if constexpr (std::is_same_v<T, MatchResumed>) {
                info.type = EventType::Resumed;
            } else if constexpr (std::is_same_v<T, MatchFinishedManually>) {
                info.type = EventType::FinishedManually;
                w.u8(e.declared_winner.has_value() ? 1 : 0);
                w.u8(e.declared_winner
                         ? static_cast<std::uint8_t>(*e.declared_winner)
                         : 0);
            } else if constexpr (std::is_same_v<T, MatchReset>) {
                info.type = EventType::Reset;
            }
        },
        event);
    return info;
}

bool valid_team(std::uint8_t value) {
    return value == static_cast<std::uint8_t>(TeamId::A) ||
           value == static_cast<std::uint8_t>(TeamId::B);
}

bool valid_source(std::uint8_t value) {
    return value >= static_cast<std::uint8_t>(InputSource::Remote) &&
           value <= static_cast<std::uint8_t>(InputSource::Simulator);
}

// Parses the event payload. Returns false on any enum/size violation.
bool parse_payload(std::uint8_t type_byte, Reader& r, std::size_t payload_length,
                   Event* out) {
    switch (static_cast<EventType>(type_byte)) {
        case EventType::Created: {
            if (payload_length != 20) return false;
            MatchCreated e{};
            e.match_id = r.u64();
            e.config = read_config(r);
            if (static_cast<std::uint8_t>(e.config.game_rule) > 1 ||
                static_cast<std::uint8_t>(e.config.final_set_rule) > 1) {
                return false;
            }
            *out = e;
            return true;
        }
        case EventType::Started: {
            if (payload_length != 1) return false;
            const std::uint8_t team = r.u8();
            if (!valid_team(team)) return false;
            *out = MatchStarted{static_cast<TeamId>(team)};
            return true;
        }
        case EventType::PointAwarded_: {
            if (payload_length != 2) return false;
            const std::uint8_t team = r.u8();
            const std::uint8_t source = r.u8();
            if (!valid_team(team) || !valid_source(source)) return false;
            *out = PointAwarded{static_cast<TeamId>(team), static_cast<InputSource>(source)};
            return true;
        }
        case EventType::Undone: {
            if (payload_length != 8) return false;
            *out = ScoringActionUndone{r.u64()};
            return true;
        }
        case EventType::ServingChanged: {
            if (payload_length != 1) return false;
            const std::uint8_t team = r.u8();
            if (!valid_team(team)) return false;
            *out = ServingTeamChanged{static_cast<TeamId>(team)};
            return true;
        }
        case EventType::Paused:
            if (payload_length != 0) return false;
            *out = MatchPaused{};
            return true;
        case EventType::Resumed:
            if (payload_length != 0) return false;
            *out = MatchResumed{};
            return true;
        case EventType::FinishedManually: {
            if (payload_length != 2) return false;
            const std::uint8_t has_winner = r.u8();
            const std::uint8_t team = r.u8();
            MatchFinishedManually e{};
            if (has_winner != 0) {
                if (!valid_team(team)) return false;
                e.declared_winner = static_cast<TeamId>(team);
            }
            *out = e;
            return true;
        }
        case EventType::Reset:
            if (payload_length != 0) return false;
            *out = MatchReset{};
            return true;
        default:
            return false;
    }
}

}  // namespace

std::vector<std::uint8_t> serialize_record(const CommittedEvent& event) {
    const PayloadInfo payload = serialize_payload(event.payload);
    const std::uint16_t record_length =
        static_cast<std::uint16_t>(kRecordOverhead + payload.bytes.size());

    std::vector<std::uint8_t> record;
    record.reserve(record_length);
    Writer w(record);
    w.u8(kJournalMagic0);
    w.u8(kJournalMagic1);
    w.u8(kJournalSchemaVersion);
    w.u16(record_length);
    w.u64(event.event_id);
    w.u64(event.match_id);
    w.u64(event.state_revision);
    w.u64(event.monotonic_ms);
    w.u8(static_cast<std::uint8_t>(event.source));
    w.u8(event.intent.has_value() ? 1 : 0);
    w.u32(event.intent ? event.intent->remote_id : 0);
    w.u32(event.intent ? event.intent->boot_id : 0);
    w.u32(event.intent ? event.intent->sequence : 0);
    w.u8(static_cast<std::uint8_t>(payload.type));
    w.u16(static_cast<std::uint16_t>(payload.bytes.size()));
    record.insert(record.end(), payload.bytes.begin(), payload.bytes.end());

    const std::uint16_t crc = protocol::crc16_ccitt(record.data(), record.size());
    record.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    record.push_back(static_cast<std::uint8_t>(crc >> 8));
    return record;
}

RecoveryResult recover(const std::vector<std::uint8_t>& bytes) {
    RecoveryResult result{};
    std::size_t offset = 0;

    while (offset < bytes.size()) {
        const std::size_t remaining = bytes.size() - offset;
        if (remaining < 5) {
            result.tail = TailStatus::TruncatedRecord;
            break;
        }
        if (bytes[offset] != kJournalMagic0 || bytes[offset + 1] != kJournalMagic1) {
            result.tail = TailStatus::CorruptRecord;
            break;
        }
        if (bytes[offset + 2] != kJournalSchemaVersion) {
            result.tail = TailStatus::UnsupportedSchema;
            break;
        }
        const std::uint16_t record_length = static_cast<std::uint16_t>(
            bytes[offset + 3] | (static_cast<std::uint16_t>(bytes[offset + 4]) << 8));
        if (record_length < kRecordOverhead ||
            record_length > kRecordOverhead + kMaxPayloadSize) {
            result.tail = TailStatus::CorruptRecord;
            break;
        }
        if (remaining < record_length) {
            result.tail = TailStatus::TruncatedRecord;
            break;
        }

        const std::uint8_t* record = bytes.data() + offset;
        const std::uint16_t stored_crc = static_cast<std::uint16_t>(
            record[record_length - 2] |
            (static_cast<std::uint16_t>(record[record_length - 1]) << 8));
        if (stored_crc != protocol::crc16_ccitt(record, record_length - 2)) {
            result.tail = TailStatus::CorruptRecord;
            break;
        }

        Reader r(record, 5);
        CommittedEvent event{};
        event.event_id = r.u64();
        event.match_id = r.u64();
        event.state_revision = r.u64();
        event.monotonic_ms = r.u64();
        const std::uint8_t source = r.u8();
        const std::uint8_t has_intent = r.u8();
        const std::uint32_t remote_id = r.u32();
        const std::uint32_t boot_id = r.u32();
        const std::uint32_t sequence = r.u32();
        const std::uint8_t event_type = r.u8();
        const std::uint16_t payload_length = r.u16();

        if (!valid_source(source) ||
            payload_length != record_length - kRecordOverhead) {
            result.tail = TailStatus::CorruptRecord;
            break;
        }
        event.source = static_cast<InputSource>(source);
        if (has_intent != 0) {
            event.intent = protocol::IntentIdentity{remote_id, boot_id, sequence};
        }
        if (!parse_payload(event_type, r, payload_length, &event.payload)) {
            result.tail = TailStatus::CorruptRecord;
            break;
        }

        result.events.push_back(std::move(event));
        offset += record_length;
    }

    result.valid_bytes = offset;
    return result;
}

JournalWriter::JournalWriter(IFileBackend& backend, std::size_t committed_size)
    : backend_(backend), committed_size_(committed_size) {}

bool JournalWriter::append(const application::CommittedEvent& event) {
    const std::vector<std::uint8_t> record = serialize_record(event);
    if (!backend_.append(record.data(), record.size()) || !backend_.sync()) {
        // Roll back so a partial record cannot sit in the middle of the file
        // once storage recovers and later appends succeed.
        backend_.truncate(committed_size_);
        return false;
    }
    committed_size_ += record.size();
    return true;
}

}  // namespace padel::persistence
