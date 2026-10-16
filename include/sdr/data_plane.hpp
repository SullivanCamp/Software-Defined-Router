// The fast path: one std::jthread per port, each blocking on its own socket.
//
// Threads share nothing but the published tables, so adding a port adds
// throughput rather than lock contention.
#pragma once

#include <cstdint>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

#include "sdr/router.hpp"

namespace sdr {

class DataPlane {
public:
    explicit DataPlane(Router& router) : router_(router) {}
    ~DataPlane() { stop(); }

    DataPlane(const DataPlane&)            = delete;
    DataPlane& operator=(const DataPlane&) = delete;

    // Spawns one worker per registered port.
    bool start();

    // Requests stop and joins. Idempotent; also runs from the destructor.
    void stop() noexcept;

private:
    void run_port(std::stop_token token, PortId port);
    void forward(Interface& ingress, const Qsbr::Reader& reader,
                 std::span<std::uint8_t> frame);

    Router&                   router_;
    std::vector<std::jthread> workers_;
};

}  // namespace sdr
