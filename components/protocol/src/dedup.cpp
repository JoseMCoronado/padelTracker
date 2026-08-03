#include "padel/protocol/dedup.hpp"

namespace padel::protocol {
namespace {

// Wrap-safe serial-number comparison (RFC 1982 style): positive when `a` is
// ahead of `b`, handling uint32 wraparound.
std::int32_t sequence_delta(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::int32_t>(a - b);
}

}  // namespace

const Deduplicator::RemoteWatermark* Deduplicator::find(std::uint32_t remote_id) const {
    for (std::size_t i = 0; i < entry_count_; ++i) {
        if (entries_[i].remote_id == remote_id) {
            return &entries_[i];
        }
    }
    return nullptr;
}

DedupResult Deduplicator::classify(const IntentIdentity& identity) const {
    const RemoteWatermark* entry = find(identity.remote_id);
    if (entry == nullptr) {
        // Unknown remote: new if we have capacity. Pairing/allow-list checks
        // happen upstream in the application layer.
        return entry_count_ < kMaxRemotes ? DedupResult::New : DedupResult::Stale;
    }
    if (entry->boot_id != identity.boot_id) {
        // Remotes generate a random boot_id each boot; a different boot_id
        // starts a fresh sequence space.
        return DedupResult::New;
    }
    const std::int32_t delta = sequence_delta(identity.sequence, entry->highest_sequence);
    if (delta > 0) {
        return DedupResult::New;
    }
    if (static_cast<std::uint32_t>(-delta) <= duplicate_window_) {
        return DedupResult::Duplicate;
    }
    return DedupResult::Stale;
}

void Deduplicator::record(const IntentIdentity& identity) {
    for (std::size_t i = 0; i < entry_count_; ++i) {
        if (entries_[i].remote_id == identity.remote_id) {
            entries_[i].boot_id = identity.boot_id;
            entries_[i].highest_sequence = identity.sequence;
            return;
        }
    }
    if (entry_count_ < kMaxRemotes) {
        entries_[entry_count_++] =
            RemoteWatermark{identity.remote_id, identity.boot_id, identity.sequence};
    }
}

DedupResult Deduplicator::check_and_record(const IntentIdentity& identity) {
    const DedupResult result = classify(identity);
    switch (result) {
        case DedupResult::New:
            record(identity);
            ++counters_.accepted;
            break;
        case DedupResult::Duplicate:
            ++counters_.duplicates;
            break;
        case DedupResult::Stale:
            ++counters_.stale;
            break;
    }
    return result;
}

std::vector<Deduplicator::RemoteWatermark> Deduplicator::snapshot() const {
    return std::vector<RemoteWatermark>(entries_.begin(), entries_.begin() + entry_count_);
}

void Deduplicator::restore(const std::vector<RemoteWatermark>& watermarks) {
    entry_count_ = 0;
    for (const RemoteWatermark& watermark : watermarks) {
        if (entry_count_ >= kMaxRemotes) {
            break;
        }
        entries_[entry_count_++] = watermark;
    }
}

}  // namespace padel::protocol
