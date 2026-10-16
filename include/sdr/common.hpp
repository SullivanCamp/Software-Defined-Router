// Project-wide limits and small shared vocabulary types.
#pragma once

#include <cstddef>
#include <cstdint>

namespace sdr {

inline constexpr std::size_t kMaxInterfaces   = 8;
inline constexpr std::size_t kMaxNeighbors    = 256;
inline constexpr std::size_t kMaxFrameBytes   = 2048;
inline constexpr std::size_t kMaxControlClients = 16;
inline constexpr std::size_t kMaxControlLine  = 4096;

// Index of an interface within the router's port table.
using PortId = std::size_t;
inline constexpr PortId kInvalidPort = static_cast<PortId>(-1);

}  // namespace sdr
