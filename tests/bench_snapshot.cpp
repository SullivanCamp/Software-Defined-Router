// Measures the read-side cost of the two ways to publish an immutable snapshot
// in C++:
//
//   A. Published<RouteTable>  - QSBR: two relaxed-ish atomic stores and a load
//   B. std::atomic<std::shared_ptr<const RouteTable>> - the idiomatic C++20
//      answer, which reclaims automatically but touches a refcount per read
//
// The port kept QSBR. This is the evidence for that choice rather than an
// assertion about it, and it prints whether atomic<shared_ptr> is even
// lock-free on this toolchain, since that varies by standard library.
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "sdr/published.hpp"
#include "sdr/route_table.hpp"

using namespace sdr;
using namespace std::chrono_literals;

namespace {

constexpr int  kReaderThreads = 4;
constexpr auto kDuration      = 2s;

Ipv4Address ip(std::string_view t) { return *Ipv4Address::parse(t); }
Prefix      pfx(std::string_view t) { return *Prefix::parse(t); }

void populate(RouteTable& table) {
    for (std::uint32_t i = 0; i < 256; ++i) {
        table.insert(Route{Prefix{Ipv4Address{ip("10.0.0.0").host() + (i << 8)}, 24},
                           i % 4, std::nullopt});
    }
    table.insert(Route{pfx("172.16.0.0/12"), 42, std::nullopt});
}

double run_qsbr() {
    Qsbr qsbr;
    Published<RouteTable> routes(qsbr);
    routes.update([](RouteTable& t) { populate(t); return true; });

    std::atomic<std::uint64_t> total{0};
    const Ipv4Address key = ip("172.16.5.5");

    {
        std::vector<std::jthread> readers;
        for (int i = 0; i < kReaderThreads; ++i) {
            readers.emplace_back([&](std::stop_token token) {
                Qsbr::Reader reader = qsbr.register_reader();
                std::uint64_t local = 0;
                while (!token.stop_requested()) {
                    for (int j = 0; j < 1024; ++j) {
                        const auto snapshot = routes.read(reader);
                        if (snapshot->lookup(key)) {
                            ++local;
                        }
                    }
                }
                total.fetch_add(local, std::memory_order_relaxed);
            });
        }
        std::this_thread::sleep_for(kDuration);
    }
    return static_cast<double>(total.load()) / 2.0;
}

double run_atomic_shared_ptr() {
    auto initial = std::make_shared<RouteTable>();
    populate(*initial);
    std::atomic<std::shared_ptr<const RouteTable>> routes{initial};

    std::atomic<std::uint64_t> total{0};
    const Ipv4Address key = ip("172.16.5.5");

    {
        std::vector<std::jthread> readers;
        for (int i = 0; i < kReaderThreads; ++i) {
            readers.emplace_back([&](std::stop_token token) {
                std::uint64_t local = 0;
                while (!token.stop_requested()) {
                    for (int j = 0; j < 1024; ++j) {
                        const auto snapshot = routes.load(std::memory_order_acquire);
                        if (snapshot->lookup(key)) {
                            ++local;
                        }
                    }
                }
                total.fetch_add(local, std::memory_order_relaxed);
            });
        }
        std::this_thread::sleep_for(kDuration);
    }
    return static_cast<double>(total.load()) / 2.0;
}

}  // namespace

int main() {
    std::atomic<std::shared_ptr<const RouteTable>> probe;
    std::cout << "hardware threads:            " << std::thread::hardware_concurrency()
              << "\nreader threads:              " << kReaderThreads
              << "\natomic<shared_ptr> lock-free: "
              << (probe.is_lock_free() ? "yes" : "no") << "\n\n";

    const double shared_ptr_rate = run_atomic_shared_ptr();
    const double qsbr_rate       = run_qsbr();

    std::cout << std::fixed << std::setprecision(2)
              << "atomic<shared_ptr>  " << std::setw(12)
              << shared_ptr_rate / 1e6 << " M lookups/sec\n"
              << "Published<T> (QSBR) " << std::setw(12)
              << qsbr_rate / 1e6 << " M lookups/sec\n\n"
              << "QSBR is " << std::setprecision(1)
              << (qsbr_rate / shared_ptr_rate) << "x the read throughput.\n";
    return 0;
}
