#include "sdr/route_table.hpp"

namespace sdr {
namespace {

// Bit 0 is the most significant bit of the address.
constexpr std::size_t bit_at(std::uint32_t value, std::uint8_t index) noexcept {
    return (value >> (31 - index)) & 1u;
}

}  // namespace

RouteTable::RouteTable() : root_(std::make_unique<Node>()) {}

RouteTable::~RouteTable() = default;

std::unique_ptr<RouteTable::Node> RouteTable::clone_node(const Node* node) {
    if (node == nullptr) {
        return nullptr;
    }
    auto copy   = std::make_unique<Node>();
    copy->route = node->route;
    copy->child[0] = clone_node(node->child[0].get());
    copy->child[1] = clone_node(node->child[1].get());
    return copy;
}

RouteTable::RouteTable(const RouteTable& other)
    : root_(clone_node(other.root_.get())), count_(other.count_) {}

RouteTable& RouteTable::operator=(const RouteTable& other) {
    if (this != &other) {
        root_  = clone_node(other.root_.get());
        count_ = other.count_;
    }
    return *this;
}

RouteTable::RouteTable(RouteTable&&) noexcept            = default;
RouteTable& RouteTable::operator=(RouteTable&&) noexcept = default;

bool RouteTable::insert(const Route& route) {
    Node* node = root_.get();
    const std::uint32_t network = route.prefix.network().host();

    for (std::uint8_t depth = 0; depth < route.prefix.length(); ++depth) {
        auto& next = node->child[bit_at(network, depth)];
        if (!next) {
            next = std::make_unique<Node>();
        }
        node = next.get();
    }

    const bool is_new = !node->route.has_value();
    node->route = route;
    if (is_new) {
        ++count_;
    }
    return is_new;
}

// Returns true if `node` became prunable, so the parent can drop it.
bool RouteTable::erase_at(Node* node, const Prefix& prefix, std::uint8_t depth,
                          bool& removed) {
    if (node == nullptr) {
        return false;
    }

    if (depth == prefix.length()) {
        if (node->route.has_value()) {
            node->route.reset();
            removed = true;
        }
    } else {
        auto& next = node->child[bit_at(prefix.network().host(), depth)];
        if (erase_at(next.get(), prefix, depth + 1, removed)) {
            next.reset();
        }
    }

    return !node->route.has_value() && !node->child[0] && !node->child[1] &&
           depth > 0;   // never prune the root
}

bool RouteTable::erase(const Prefix& prefix) {
    bool removed = false;
    erase_at(root_.get(), prefix, 0, removed);
    if (removed) {
        --count_;
    }
    return removed;
}

std::optional<Route> RouteTable::lookup(Ipv4Address destination) const noexcept {
    const Node*          node = root_.get();
    std::optional<Route> best;
    const std::uint32_t  key = destination.host();

    for (std::uint8_t depth = 0; node != nullptr; ++depth) {
        if (node->route.has_value()) {
            best = node->route;      // deeper hit is a longer prefix: LPM
        }
        if (depth == 32) {
            break;
        }
        node = node->child[bit_at(key, depth)].get();
    }
    return best;
}

void RouteTable::collect(const Node* node, std::vector<Route>& out) {
    if (node == nullptr) {
        return;
    }
    if (node->route.has_value()) {
        out.push_back(*node->route);
    }
    collect(node->child[0].get(), out);
    collect(node->child[1].get(), out);
}

std::vector<Route> RouteTable::dump() const {
    std::vector<Route> out;
    out.reserve(count_);
    collect(root_.get(), out);
    return out;
}

}  // namespace sdr
