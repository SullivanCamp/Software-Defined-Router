#include "sdr/neighbor_table.hpp"

#include <algorithm>

#include "sdr/common.hpp"

namespace sdr {

bool NeighborTable::insert(Ipv4Address ip, const MacAddress& mac) {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [ip](const NeighborEntry& e) { return e.ip == ip; });
    if (it != entries_.end()) {
        it->mac = mac;
        return false;
    }
    if (entries_.size() >= kMaxNeighbors) {
        return false;
    }
    entries_.push_back(NeighborEntry{ip, mac});
    return true;
}

bool NeighborTable::erase(Ipv4Address ip) {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [ip](const NeighborEntry& e) { return e.ip == ip; });
    if (it == entries_.end()) {
        return false;
    }
    entries_.erase(it);
    return true;
}

std::optional<MacAddress> NeighborTable::lookup(Ipv4Address ip) const noexcept {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [ip](const NeighborEntry& e) { return e.ip == ip; });
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return it->mac;
}

}  // namespace sdr
