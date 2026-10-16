// Exercises Published<T> the way the router actually uses it: several reader
// threads doing lookups while a writer continuously installs and withdraws
// routes.
//
// What this proves:
//   1. No reader ever observes a torn or freed table (run under ASan/TSan).
//   2. A route the writer never touches is visible on EVERY lookup, because the
//      swap is atomic and there is no window where the table looks empty.
//   3. Published<T>::update aborting leaves the published table untouched.
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "sdr/published.hpp"
#include "sdr/route_table.hpp"
#include "test_support.hpp"

using namespace sdr;
using namespace std::chrono_literals;

namespace {

constexpr int kReaders = 4;
constexpr auto kDuration = 2s;

Ipv4Address ip(std::string_view text) { return *Ipv4Address::parse(text); }
Prefix      pfx(std::string_view text) { return *Prefix::parse(text); }

std::atomic<std::uint64_t> g_lookups{0};
std::atomic<std::uint64_t> g_updates{0};
std::atomic<std::uint64_t> g_errors{0};

void reader_loop(std::stop_token token, Qsbr& qsbr, Published<RouteTable>& routes) {
    Qsbr::Reader reader = qsbr.register_reader();
    const Ipv4Address stable = ip("172.16.5.5");
    std::uint64_t local = 0;

    while (!token.stop_requested()) {
        for (int i = 0; i < 256; ++i) {
            const auto snapshot = routes.read(reader);

            // The invariant: a route nobody is modifying is always there.
            const auto found = snapshot->lookup(stable);
            if (!found || found->out_port != 42) {
                g_errors.fetch_add(1, std::memory_order_relaxed);
            }

            // Churning prefixes: any answer is fine, but it must be coherent.
            const auto churn = snapshot->lookup(
                Ipv4Address{ip("10.0.0.1").host() + static_cast<std::uint32_t>(i & 0xFF)});
            if (churn && churn->prefix.length() > 32) {
                g_errors.fetch_add(1, std::memory_order_relaxed);
            }
            ++local;
        }
    }
    g_lookups.fetch_add(local, std::memory_order_relaxed);
}

void writer_loop(std::stop_token token, Published<RouteTable>& routes) {
    std::uint64_t count = 0;

    while (!token.stop_requested()) {
        for (std::uint32_t i = 0; i < 16; ++i) {
            const Prefix prefix{Ipv4Address{ip("10.0.0.0").host() + (i << 8)}, 24};
            routes.update([&](RouteTable& table) {
                table.insert(Route{prefix, i, std::nullopt});
                return true;
            });
            ++count;
        }
        for (std::uint32_t i = 0; i < 16; ++i) {
            const Prefix prefix{Ipv4Address{ip("10.0.0.0").host() + (i << 8)}, 24};
            if (!routes.update([&](RouteTable& table) { return table.erase(prefix); })) {
                g_errors.fetch_add(1, std::memory_order_relaxed);
            }
            ++count;
        }
    }
    g_updates.fetch_add(count, std::memory_order_relaxed);
}

void test_aborted_update_publishes_nothing() {
    test::section("an aborted update leaves the table untouched");
    Qsbr qsbr;
    Published<RouteTable> routes(qsbr);

    routes.update([](RouteTable& t) {
        t.insert(Route{pfx("10.0.0.0/8"), 1, std::nullopt});
        return true;
    });

    // Erasing a prefix that is not present returns false, aborting the update.
    const bool published = routes.update(
        [](RouteTable& t) { return t.erase(pfx("99.0.0.0/8")); });
    CHECK(!published);

    const auto size = routes.inspect([](const RouteTable& t) { return t.size(); });
    CHECK_EQ(size, std::size_t{1});
}

void test_concurrent_read_write() {
    std::cout << "-- " << kReaders << " readers vs 1 writer for 2 seconds\n";

    Qsbr qsbr;
    Published<RouteTable> routes(qsbr);

    routes.update([](RouteTable& table) {
        table.insert(Route{pfx("172.16.0.0/12"), 42, std::nullopt});
        return true;
    });

    {
        std::vector<std::jthread> readers;
        readers.reserve(kReaders);
        for (int i = 0; i < kReaders; ++i) {
            readers.emplace_back([&](std::stop_token t) {
                reader_loop(std::move(t), qsbr, routes);
            });
        }
        std::jthread writer(
            [&](std::stop_token t) { writer_loop(std::move(t), routes); });

        std::this_thread::sleep_for(kDuration);
        // jthreads request stop and join on destruction.
    }

    const auto lookups = g_lookups.load();
    const auto updates = g_updates.load();
    const auto errors  = g_errors.load();

    std::cout << "   " << lookups << " lookups, " << updates
              << " table updates, " << errors << " errors\n";

    CHECK_EQ(errors, std::uint64_t{0});
    CHECK(lookups > 0);
    CHECK(updates > 0);
}

}  // namespace

int main() {
    test_aborted_update_publishes_nothing();
    test_concurrent_read_write();
    return test::summary();
}
