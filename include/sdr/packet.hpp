// Wire formats, validation, and checksum arithmetic.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>

#include "sdr/address.hpp"

namespace sdr {

inline constexpr std::size_t   kEthHeaderLen = 14;
inline constexpr std::uint16_t kEtherTypeIpv4 = 0x0800;
inline constexpr std::uint16_t kEtherTypeArp  = 0x0806;
inline constexpr std::size_t   kIpv4MinHeaderLen = 20;

struct EthHeader {
    std::uint8_t  dst[MacAddress::kSize];
    std::uint8_t  src[MacAddress::kSize];
    std::uint16_t ethertype;   // network order
} __attribute__((packed));

struct Ipv4Header {
    std::uint8_t  version_ihl;
    std::uint8_t  tos;
    std::uint16_t total_length;  // network order
    std::uint16_t id;
    std::uint16_t frag_offset;
    std::uint8_t  ttl;
    std::uint8_t  protocol;
    std::uint16_t checksum;      // network order
    std::uint32_t src;           // network order
    std::uint32_t dst;           // network order

    [[nodiscard]] std::uint8_t version() const noexcept { return version_ihl >> 4; }
    [[nodiscard]] std::size_t  header_length() const noexcept {
        return static_cast<std::size_t>(version_ihl & 0x0F) * 4;
    }
} __attribute__((packed));

enum class PacketError {
    Runt,
    NotIpv4,
    BadVersion,
    BadHeaderLength,
    BadTotalLength,
    BadChecksum,
    TtlExpired,
};

std::string_view to_string(PacketError error);

// A validated view into a caller-owned frame buffer.
struct ParsedFrame {
    EthHeader*  eth{nullptr};
    Ipv4Header* ip{nullptr};
    Ipv4Address destination{};
    Ipv4Address source{};
};

// Either a parsed frame or the reason it was rejected. Using a variant makes
// the failure impossible to ignore: there is no "valid" flag a caller can
// forget to check before dereferencing.
using ParseResult = std::variant<ParsedFrame, PacketError>;

[[nodiscard]] ParseResult parse_frame(std::span<std::uint8_t> frame) noexcept;

// Standard one's-complement checksum. Result is in network order.
[[nodiscard]] std::uint16_t ip_checksum(std::span<const std::uint8_t> data) noexcept;

// RFC 1624 incremental update: swap one 16-bit word, all values network order.
[[nodiscard]] std::uint16_t checksum_replace(std::uint16_t checksum,
                                             std::uint16_t old_word,
                                             std::uint16_t new_word) noexcept;

// Decrements TTL and patches the header checksum incrementally.
void decrement_ttl(Ipv4Header& ip) noexcept;

}  // namespace sdr
