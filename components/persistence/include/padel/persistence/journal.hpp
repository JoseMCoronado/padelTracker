#pragma once

#include <cstdint>
#include <vector>

#include "padel/application/event_store.hpp"
#include "padel/persistence/file_backend.hpp"

namespace padel::persistence {

// Append-only CRC-framed binary journal (ADR-0005, spec section 13.2).
//
// Record layout (little-endian):
//   magic            2   "PJ"
//   schema_version   1   1
//   record_length    2   total bytes including magic and crc
//   event_id         8
//   match_id         8
//   state_revision   8
//   monotonic_ms     8
//   source           1   InputSource
//   has_intent       1
//   remote_id        4   0 when has_intent == 0
//   boot_id          4
//   sequence         4
//   event_type       1
//   payload_length   2
//   payload          var event-specific fields
//   crc16            2   CRC16/CCITT-FALSE over all preceding bytes

inline constexpr std::uint8_t kJournalMagic0 = 'P';
inline constexpr std::uint8_t kJournalMagic1 = 'J';
inline constexpr std::uint8_t kJournalSchemaVersion = 1;
inline constexpr std::size_t kRecordHeaderSize = 54;
inline constexpr std::size_t kRecordOverhead = kRecordHeaderSize + 2;  // + crc
inline constexpr std::size_t kMaxPayloadSize = 64;

// Serializes one committed event into a journal record.
std::vector<std::uint8_t> serialize_record(const application::CommittedEvent& event);

enum class TailStatus : std::uint8_t {
    Clean,              // journal parsed to the end
    TruncatedRecord,    // partial record at the tail (torn write / power loss)
    CorruptRecord,      // framing/CRC/enum damage at the tail
    UnsupportedSchema,  // record with a newer schema version
};

struct RecoveryResult {
    std::vector<application::CommittedEvent> events{};
    TailStatus tail = TailStatus::Clean;
    // Bytes of journal that parsed cleanly. Boot code truncates the backend
    // to this size before appending new records (never silently resets the
    // match, spec section 12.2 - the corrupt tail is reported, not hidden).
    std::size_t valid_bytes = 0;
};

// Replays a journal byte stream up to the last valid record.
RecoveryResult recover(const std::vector<std::uint8_t>& bytes);

// Durable writer: every append is written and fsync'd before it reports
// success; a failed append rolls the file back to the last committed size so
// a later append cannot create an interleaved corrupt region.
class JournalWriter {
public:
    // committed_size: end of the valid journal region (RecoveryResult::
    // valid_bytes at boot, 0 for a fresh file).
    JournalWriter(IFileBackend& backend, std::size_t committed_size);

    bool append(const application::CommittedEvent& event);

    std::size_t committed_size() const { return committed_size_; }

private:
    IFileBackend& backend_;
    std::size_t committed_size_;
};

// Adapter: the application's IEventStore backed by the journal.
class JournalEventStore : public application::IEventStore {
public:
    JournalEventStore(IFileBackend& backend, std::size_t committed_size)
        : writer_(backend, committed_size) {}

    bool append(const application::CommittedEvent& event) override {
        return writer_.append(event);
    }

    const JournalWriter& writer() const { return writer_; }

private:
    JournalWriter writer_;
};

}  // namespace padel::persistence
