// Quiescent-state-based reclamation.
//
// Lets the control plane retire a data structure that data-plane threads may
// still be reading, without those readers taking a lock or performing an atomic
// read-modify-write on the packet path.
//
// Protocol:
//   reader  begin() -> load pointer -> use -> end()
//   writer  publish new pointer (release) -> synchronize() -> delete old
//
// A reader publishes the epoch it observed on entry and zeroes it on exit. A
// writer bumps the epoch, then waits until every reader is either quiescent (0)
// or at an epoch at least as new as the bump. Such a reader cannot still hold
// the old pointer, so it is safe to free.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace sdr {

class Qsbr {
public:
    static constexpr std::size_t kMaxReaders = 16;

    Qsbr() = default;
    Qsbr(const Qsbr&)            = delete;
    Qsbr& operator=(const Qsbr&) = delete;

    // A reader's registration. Non-owning and cheap to copy, but only valid
    // while the Qsbr it came from is alive.
    class Reader {
    public:
        Reader() noexcept = default;

        void begin() const noexcept {
            const std::uint64_t e = owner_->epoch_.load(std::memory_order_acquire);
            // seq_cst: this store must be globally visible before the reader
            // loads the protected pointer, or a writer could miss it.
            owner_->readers_[index_].store(e, std::memory_order_seq_cst);
        }

        void end() const noexcept {
            owner_->readers_[index_].store(0, std::memory_order_release);
        }

        [[nodiscard]] bool valid() const noexcept { return owner_ != nullptr; }

    private:
        friend class Qsbr;
        Reader(Qsbr* owner, std::size_t index) noexcept
            : owner_(owner), index_(index) {}

        Qsbr*       owner_{nullptr};
        std::size_t index_{0};
    };

    // Scope guard. Pairing begin() with end() by hand is the one thing a reader
    // can get wrong; this makes it structural, including on early return.
    class Guard {
    public:
        explicit Guard(const Reader& reader) noexcept : reader_(reader) {
            reader_.begin();
        }
        ~Guard() { reader_.end(); }

        Guard(const Guard&)            = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&&)                 = delete;
        Guard& operator=(Guard&&)      = delete;

    private:
        const Reader& reader_;
    };

    [[nodiscard]] Reader register_reader();

    // Blocks until no reader can still hold a pointer published before the call.
    void synchronize() noexcept;

    [[nodiscard]] std::size_t reader_count() const noexcept {
        return count_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t>                              epoch_{1};
    std::array<std::atomic<std::uint64_t>, kMaxReaders>     readers_{};
    std::atomic<std::size_t>                                count_{0};
};

}  // namespace sdr
