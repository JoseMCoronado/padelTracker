#pragma once

#include <string>

#include "padel/persistence/file_backend.hpp"

namespace padel::persistence {

// POSIX-file implementation of IFileBackend. Works on the host (court-sim,
// tests against real files) and unchanged on the device over an ESP-IDF
// VFS-mounted LittleFS partition (open/write/fsync/ftruncate are all
// supported there), which makes it the firmware journal backend too.
class StdioFileBackend : public IFileBackend {
public:
    explicit StdioFileBackend(std::string path);
    ~StdioFileBackend() override;

    StdioFileBackend(const StdioFileBackend&) = delete;
    StdioFileBackend& operator=(const StdioFileBackend&) = delete;

    bool ok() const { return fd_ >= 0; }
    const std::string& path() const { return path_; }

    bool append(const std::uint8_t* data, std::size_t length) override;
    bool sync() override;
    bool truncate(std::size_t size) override;
    std::vector<std::uint8_t> read_all() const override;
    std::size_t size() const override;

private:
    std::string path_;
    int fd_ = -1;
    std::size_t size_ = 0;
};

}  // namespace padel::persistence
