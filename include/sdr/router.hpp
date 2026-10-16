// Ties the pieces together and owns their lifetimes.
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "sdr/interface.hpp"
#include "sdr/neighbor_table.hpp"
#include "sdr/published.hpp"
#include "sdr/qsbr.hpp"
#include "sdr/route_table.hpp"

namespace sdr {

class Router {
public:
    Router() : routes_(qsbr_), neighbors_(qsbr_) {}

    Router(const Router&)            = delete;
    Router& operator=(const Router&) = delete;

    [[nodiscard]] Qsbr&                        qsbr()      noexcept { return qsbr_; }
    [[nodiscard]] PortTable&                   ports()     noexcept { return ports_; }
    [[nodiscard]] const PortTable&             ports() const noexcept { return ports_; }
    [[nodiscard]] Published<RouteTable>&       routes()    noexcept { return routes_; }
    [[nodiscard]] Published<NeighborTable>&    neighbors() noexcept { return neighbors_; }

private:
    Qsbr                     qsbr_;
    PortTable                ports_;
    Published<RouteTable>    routes_;
    Published<NeighborTable> neighbors_;
};

}  // namespace sdr
