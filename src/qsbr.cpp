#include "sdr/qsbr.hpp"

#include <sched.h>

#include <chrono>
#include <thread>

namespace sdr {

Qsbr::Reader Qsbr::register_reader() {
    const std::size_t index = count_.fetch_add(1, std::memory_order_relaxed);
    if (index >= kMaxReaders) {
        count_.fetch_sub(1, std::memory_order_relaxed);
        throw std::runtime_error("QSBR reader limit exceeded");
    }
    readers_[index].store(0, std::memory_order_relaxed);
    return Reader{this, index};
}

void Qsbr::synchronize() noexcept {
    // The caller has already published the new pointer with release ordering.
    const std::uint64_t target =
        epoch_.fetch_add(1, std::memory_order_seq_cst) + 1;

    const std::size_t readers = count_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < readers && i < kMaxReaders; ++i) {
        unsigned spins = 0;
        while (true) {
            const std::uint64_t observed =
                readers_[i].load(std::memory_order_seq_cst);
            if (observed == 0 || observed >= target) {
                break;   // quiescent, or entered after our bump
            }
            if (++spins < 64) {
                ::sched_yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    }
}

}  // namespace sdr
