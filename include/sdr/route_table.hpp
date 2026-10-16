// Longest-prefix-match forwarding table backed by a binary trie.
//
// The table is a plain value type with deep-copy semantics: copying it clones
// it. That is what lets Published<RouteTable> do its job without the table
// knowing anything about threads.
//
// lookup() returns std::optional<Route> by value rather than a pointer into the
// trie. In the C version the caller had to remember to copy the route out
// before releasing the RCU snapshot; returning by value removes the footgun.
#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "sdr/address.hpp"
#include "sdr/common.hpp"

namespace sdr {

struct Route {
    Prefix                     prefix;
    PortId                     out_port{kInvalidPort};
    std::optional<Ipv4Address> next_hop;   // nullopt == directly connected

    friend bool operator==(const Route&, const Route&) noexcept = default;
};

class RouteTable {
public:
    RouteTable();
    ~RouteTable();

    RouteTable(const RouteTable& other);                   // deep clone
    RouteTable& operator=(const RouteTable& other);
    RouteTable(RouteTable&&) noexcept;
    RouteTable& operator=(RouteTable&&) noexcept;

    // Inserting an existing prefix replaces it. Returns true if a new prefix
    // was added, false if an existing one was overwritten.
    bool insert(const Route& route);

    // Returns false if the prefix was not present.
    bool erase(const Prefix& prefix);

    [[nodiscard]] std::optional<Route> lookup(Ipv4Address destination) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] bool        empty() const noexcept { return count_ == 0; }

    [[nodiscard]] std::vector<Route> dump() const;

private:
    struct Node {
        std::array<std::unique_ptr<Node>, 2> child;
        std::optional<Route>                 route;
    };

    static std::unique_ptr<Node> clone_node(const Node* node);
    static void                  collect(const Node* node, std::vector<Route>& out);
    static bool                  erase_at(Node* node, const Prefix& prefix,
                                          std::uint8_t depth, bool& removed);

    std::unique_ptr<Node> root_;
    std::size_t           count_{0};
};

}  // namespace sdr
