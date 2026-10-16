// Published<T>: an immutable snapshot of T that readers see without locking and
// writers replace wholesale.
//
// This template is the main structural win of the C++ port. In the C version
// the identical clone/publish/reclaim logic was written out twice, once for the
// routing table and once for the neighbor table, and the two copies had already
// started to drift. Here it exists once and both tables instantiate it.
//
// T must be copy-constructible; the copy is the clone.
#pragma once

#include <atomic>
#include <mutex>
#include <utility>

#include "sdr/qsbr.hpp"

namespace sdr {

template <typename T>
class Published {
public:
    explicit Published(Qsbr& qsbr) : qsbr_(qsbr), current_(new T()) {}

    template <typename... Args>
    Published(Qsbr& qsbr, Args&&... args)
        : qsbr_(qsbr), current_(new T(std::forward<Args>(args)...)) {}

    ~Published() { delete current_.load(std::memory_order_relaxed); }

    Published(const Published&)            = delete;
    Published& operator=(const Published&) = delete;

    // Read access for the data plane. The guard keeps the snapshot alive for
    // the lifetime of this object, so the reference is only valid while it is.
    class Snapshot {
    public:
        Snapshot(const Qsbr::Reader& reader, const std::atomic<T*>& cell) noexcept
            : guard_(reader), value_(cell.load(std::memory_order_acquire)) {}

        Snapshot(const Snapshot&)            = delete;
        Snapshot& operator=(const Snapshot&) = delete;

        [[nodiscard]] const T& operator*()  const noexcept { return *value_; }
        [[nodiscard]] const T* operator->() const noexcept { return value_; }

    private:
        Qsbr::Guard guard_;
        const T*    value_;
    };

    [[nodiscard]] Snapshot read(const Qsbr::Reader& reader) const noexcept {
        return Snapshot(reader, current_);
    }

    // Writer side. `mutate` receives a private copy and returns false to abort,
    // in which case nothing is published and the copy is discarded. Writers are
    // serialized against each other but never block readers.
    template <typename Fn>
    bool update(Fn&& mutate) {
        std::lock_guard<std::mutex> lock(write_mutex_);

        T* old_value = current_.load(std::memory_order_relaxed);
        auto next = std::make_unique<T>(*old_value);       // clone

        if (!mutate(*next)) {
            return false;                                   // nothing published
        }

        current_.store(next.release(), std::memory_order_release);
        qsbr_.synchronize();                                // wait out readers
        delete old_value;
        return true;
    }

    // Writer-side read, for control-plane queries such as SHOW ROUTES. Takes the
    // write lock rather than an RCU slot, since the control thread is not a
    // registered reader and these queries are rare.
    template <typename Fn>
    auto inspect(Fn&& fn) const {
        std::lock_guard<std::mutex> lock(write_mutex_);
        return fn(*current_.load(std::memory_order_relaxed));
    }

private:
    Qsbr&                       qsbr_;
    std::atomic<T*>             current_;
    mutable std::mutex          write_mutex_;
};

}  // namespace sdr
