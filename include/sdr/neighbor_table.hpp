// Static IP -> MAC table. A real router learns these with ARP; here they are
// installed through the control protocol.
//
// A flat vector rather than a trie: it stays small, and a linear scan of a
// handful of cache-resident entries beats a pointer chase.
#pragma once

#include <optional>
#include <vector>

#include "sdr/address.hpp"

namespace sdr {

struct NeighborEntry {
    Ipv4Address ip;
    MacAddress  mac;
};

class NeighborTable {
public:
    bool insert(Ipv4Address ip, const MacAddress& mac);   // true if newly added
    bool erase(Ipv4Address ip);

    [[nodiscard]] std::optional<MacAddress> lookup(Ipv4Address ip) const noexcept;
    [[nodiscard]] std::size_t               size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool                      empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] const std::vector<NeighborEntry>& entries() const noexcept {
        return entries_;
    }

private:
    std::vector<NeighborEntry> entries_;
};

}  // namespace sdr
