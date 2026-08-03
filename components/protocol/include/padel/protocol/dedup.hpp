#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "padel/protocol/packets.hpp"

namespace padel::protocol {

enum class DedupResult : std::uint8_t {
    New,        // first time seen; caller must process and record
    Duplicate,  // already accepted; re-ACK DuplicateAccepted, do not apply
    Stale,      // impossibly old or no capacity; reject
};

// Deduplicates point intents by (remote_id, boot_id, sequence) per ADR-0007.
// The remote protocol is stop-and-wait, so per-remote sequences arrive in
// order: a watermark plus a bounded backward window is sufficient and O(1)
// per remote. State is serializable so persistence (M3) can restore it after
// a court reboot, keeping retried packets classified as duplicates.
class Deduplicator {
public:
    struct RemoteWatermark {
        std::uint32_t remote_id = 0;
        std::uint32_t boot_id = 0;
        std::uint32_t highest_sequence = 0;
    };

    struct Counters {
        std::uint32_t accepted = 0;
        std::uint32_t duplicates = 0;
        std::uint32_t stale = 0;
    };

    static constexpr std::size_t kMaxRemotes = 8;
    static constexpr std::uint32_t kDefaultDuplicateWindow = 64;

    explicit Deduplicator(std::uint32_t duplicate_window = kDefaultDuplicateWindow)
        : duplicate_window_(duplicate_window) {}

    // Classifies the intent and, when New, records it atomically.
    DedupResult check_and_record(const IntentIdentity& identity);

    // Classification without mutation (for two-phase accept: classify,
    // durably commit, then record).
    DedupResult classify(const IntentIdentity& identity) const;
    void record(const IntentIdentity& identity);

    const Counters& counters() const { return counters_; }

    std::vector<RemoteWatermark> snapshot() const;
    void restore(const std::vector<RemoteWatermark>& watermarks);

private:
    const RemoteWatermark* find(std::uint32_t remote_id) const;

    std::array<RemoteWatermark, kMaxRemotes> entries_{};
    std::size_t entry_count_ = 0;
    std::uint32_t duplicate_window_;
    Counters counters_{};
};

}  // namespace padel::protocol
