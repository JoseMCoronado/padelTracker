#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace padel::persistence {

// Storage abstraction under the journal. The firmware implementation will be
// a LittleFS-backed file; native tests use InMemoryFileBackend with fault
// injection to run the power-loss matrix (spec section 13.5).
class IFileBackend {
public:
    virtual ~IFileBackend() = default;

    // Appends bytes at the end. Returning true means the bytes were written
    // (not necessarily durable until sync()).
    virtual bool append(const std::uint8_t* data, std::size_t length) = 0;

    // Makes all written bytes durable. Returning true is the durability
    // guarantee the ACK path relies on.
    virtual bool sync() = 0;

    // Rolls the file back to `size` bytes (recovery after a failed append,
    // or truncating a corrupt tail found at boot).
    virtual bool truncate(std::size_t size) = 0;

    virtual std::vector<std::uint8_t> read_all() const = 0;
    virtual std::size_t size() const = 0;
};

// Native fake with fault injection.
class InMemoryFileBackend : public IFileBackend {
public:
    bool append(const std::uint8_t* data, std::size_t length) override {
        if (fail_appends_) {
            return false;
        }
        if (tear_next_append_bytes_) {
            const std::size_t accepted = *tear_next_append_bytes_ < length
                                             ? *tear_next_append_bytes_
                                             : length;
            data_.insert(data_.end(), data, data + accepted);
            tear_next_append_bytes_.reset();
            return false;  // caller sees failure; partial bytes are on disk
        }
        data_.insert(data_.end(), data, data + length);
        return true;
    }

    bool sync() override {
        if (fail_syncs_) {
            return false;
        }
        synced_size_ = data_.size();
        return true;
    }

    bool truncate(std::size_t size) override {
        if (size < data_.size()) {
            data_.resize(size);
        }
        if (synced_size_ > data_.size()) {
            synced_size_ = data_.size();
        }
        return true;
    }

    std::vector<std::uint8_t> read_all() const override { return data_; }
    std::size_t size() const override { return data_.size(); }

    // --- Fault injection ----------------------------------------------------
    void fail_appends(bool fail) { fail_appends_ = fail; }
    void fail_syncs(bool fail) { fail_syncs_ = fail; }
    // The next append accepts only `bytes` bytes, then reports failure
    // (simulates power dying mid-write: a torn record on disk).
    void tear_next_append(std::size_t bytes) { tear_next_append_bytes_ = bytes; }
    // Power loss: everything not fsync'd is gone.
    void simulate_power_loss() { data_.resize(synced_size_); }

    std::size_t durable_size() const { return synced_size_; }

    // Direct corruption for recovery tests.
    void corrupt_byte(std::size_t offset) {
        if (offset < data_.size()) {
            data_[offset] ^= 0xFF;
            if (synced_size_ < data_.size()) {
                synced_size_ = data_.size();
            }
        }
    }

private:
    std::vector<std::uint8_t> data_{};
    std::size_t synced_size_ = 0;
    bool fail_appends_ = false;
    bool fail_syncs_ = false;
    std::optional<std::size_t> tear_next_append_bytes_{};
};

}  // namespace padel::persistence
