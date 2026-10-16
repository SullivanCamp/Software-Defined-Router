// Move-only RAII wrapper around a POSIX file descriptor.
//
// The C version of this project leaked a socket on every early-return path in
// interface setup. Making the descriptor an owning, non-copyable type removes
// that entire class of bug at compile time.
#pragma once

#include <unistd.h>

#include <utility>

namespace sdr {

class FileDescriptor {
public:
    FileDescriptor() noexcept = default;
    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}

    ~FileDescriptor() { reset(); }

    FileDescriptor(const FileDescriptor&)            = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}

    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] int  get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return valid(); }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

private:
    int fd_{-1};
};

}  // namespace sdr
