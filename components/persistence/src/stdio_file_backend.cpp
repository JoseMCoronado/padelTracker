#include "padel/persistence/stdio_file_backend.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

namespace padel::persistence {

StdioFileBackend::StdioFileBackend(std::string path) : path_(std::move(path)) {
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ >= 0) {
        struct stat info {};
        if (::fstat(fd_, &info) == 0) {
            size_ = static_cast<std::size_t>(info.st_size);
        }
    }
}

StdioFileBackend::~StdioFileBackend() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

bool StdioFileBackend::append(const std::uint8_t* data, std::size_t length) {
    if (fd_ < 0) {
        return false;
    }
    std::size_t written = 0;
    while (written < length) {
        const ssize_t n = ::pwrite(fd_, data + written, length - written,
                                   static_cast<off_t>(size_ + written));
        if (n <= 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    size_ += length;
    return true;
}

bool StdioFileBackend::sync() {
    return fd_ >= 0 && ::fsync(fd_) == 0;
}

bool StdioFileBackend::truncate(std::size_t size) {
    if (fd_ < 0 || size > size_) {
        return fd_ >= 0;
    }
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
        return false;
    }
    size_ = size;
    return true;
}

std::vector<std::uint8_t> StdioFileBackend::read_all() const {
    std::vector<std::uint8_t> bytes;
    if (fd_ < 0 || size_ == 0) {
        return bytes;
    }
    bytes.resize(size_);
    std::size_t done = 0;
    while (done < size_) {
        const ssize_t n = ::pread(fd_, bytes.data() + done, size_ - done,
                                  static_cast<off_t>(done));
        if (n <= 0) {
            if (errno == EINTR) {
                continue;
            }
            bytes.resize(done);
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    return bytes;
}

std::size_t StdioFileBackend::size() const {
    return size_;
}

}  // namespace padel::persistence
