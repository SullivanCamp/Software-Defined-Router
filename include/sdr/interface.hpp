// A router port: an AF_PACKET socket bound to one Linux interface, plus its
// counters.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sdr/address.hpp"
#include "sdr/common.hpp"
#include "sdr/file_descriptor.hpp"

namespace sdr {

// Every counter is written only by its own port's RX thread, so relaxed
// ordering is sufficient; only the control thread reads them.
struct PortCounters {
    std::atomic<std::uint64_t> rx_packets{0};
    std::atomic<std::uint64_t> rx_bytes{0};
    std::atomic<std::uint64_t> tx_packets{0};
    std::atomic<std::uint64_t> tx_bytes{0};
    std::atomic<std::uint64_t> tx_errors{0};
    std::atomic<std::uint64_t> forwarded{0};
    std::atomic<std::uint64_t> drop_not_for_us{0};
    std::atomic<std::uint64_t> drop_non_ipv4{0};
    std::atomic<std::uint64_t> drop_malformed{0};
    std::atomic<std::uint64_t> drop_checksum{0};
    std::atomic<std::uint64_t> drop_ttl{0};
    std::atomic<std::uint64_t> drop_no_route{0};
    std::atomic<std::uint64_t> drop_no_neighbor{0};
    std::atomic<std::uint64_t> drop_hairpin{0};

    static void bump(std::atomic<std::uint64_t>& counter) noexcept {
        counter.fetch_add(1, std::memory_order_relaxed);
    }
    static void add(std::atomic<std::uint64_t>& counter, std::uint64_t n) noexcept {
        counter.fetch_add(n, std::memory_order_relaxed);
    }
};

class Interface {
public:
    // Opens and binds an AF_PACKET socket. Returns nullptr on failure and logs
    // why; requires CAP_NET_RAW.
    static std::unique_ptr<Interface> open(std::string_view name, PortId port);

    Interface(const Interface&)            = delete;
    Interface& operator=(const Interface&) = delete;

    [[nodiscard]] const std::string& name()    const noexcept { return name_; }
    [[nodiscard]] int                index()   const noexcept { return if_index_; }
    [[nodiscard]] const MacAddress&  mac()     const noexcept { return mac_; }
    [[nodiscard]] PortId             port()    const noexcept { return port_; }
    [[nodiscard]] int                fd()      const noexcept { return socket_.get(); }
    [[nodiscard]] PortCounters&      counters() noexcept { return counters_; }
    [[nodiscard]] const PortCounters& counters() const noexcept { return counters_; }

    // Sends a fully formed L2 frame out of this port.
    bool send(std::span<const std::uint8_t> frame, const MacAddress& destination) noexcept;

private:
    Interface(std::string name, int if_index, MacAddress mac, PortId port,
              FileDescriptor socket);

    std::string    name_;
    int            if_index_{-1};
    MacAddress     mac_;
    PortId         port_{kInvalidPort};
    FileDescriptor socket_;
    PortCounters   counters_;
};

// Owns the router's ports. Interfaces are held by unique_ptr because
// PortCounters holds atomics and so cannot be relocated by vector growth.
class PortTable {
public:
    bool add(std::string_view name);

    [[nodiscard]] std::size_t size()  const noexcept { return ports_.size(); }
    [[nodiscard]] bool        empty() const noexcept { return ports_.empty(); }

    [[nodiscard]] Interface*       at(PortId id) noexcept;
    [[nodiscard]] const Interface* at(PortId id) const noexcept;
    [[nodiscard]] std::optional<PortId> find(std::string_view name) const noexcept;

    [[nodiscard]] auto begin() noexcept { return ports_.begin(); }
    [[nodiscard]] auto end()   noexcept { return ports_.end(); }
    [[nodiscard]] auto begin() const noexcept { return ports_.begin(); }
    [[nodiscard]] auto end()   const noexcept { return ports_.end(); }

private:
    std::vector<std::unique_ptr<Interface>> ports_;
};

}  // namespace sdr
