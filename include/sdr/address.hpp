// Strong value types for the addresses the router manipulates.
//
// In the C version an IPv4 address was a bare uint32_t and the byte order lived
// in a comment, which is exactly how endianness bugs happen. Here the type
// remembers: host() and network() are separate, explicit accessors and there is
// no implicit conversion to an integer.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace sdr {

class Ipv4Address {
public:
    constexpr Ipv4Address() noexcept = default;
    constexpr explicit Ipv4Address(std::uint32_t host_order) noexcept
        : value_(host_order) {}

    // Builds from a dotted-quad. Returns nullopt rather than throwing, because
    // parsing user input failing is ordinary control flow, not an exception.
    static std::optional<Ipv4Address> parse(std::string_view text);

    // Constructs from a big-endian value as it appears on the wire.
    static Ipv4Address from_network(std::uint32_t network_order) noexcept;

    [[nodiscard]] constexpr std::uint32_t host() const noexcept { return value_; }
    [[nodiscard]] std::uint32_t           network() const noexcept;
    [[nodiscard]] std::string             str() const;

    friend constexpr bool operator==(Ipv4Address, Ipv4Address) noexcept = default;
    friend constexpr auto operator<=>(Ipv4Address, Ipv4Address) noexcept = default;

private:
    std::uint32_t value_{0};
};

std::ostream& operator<<(std::ostream& os, Ipv4Address addr);

class MacAddress {
public:
    static constexpr std::size_t kSize = 6;
    using Bytes = std::array<std::uint8_t, kSize>;

    constexpr MacAddress() noexcept = default;
    constexpr explicit MacAddress(Bytes bytes) noexcept : bytes_(bytes) {}

    static std::optional<MacAddress> parse(std::string_view text);

    [[nodiscard]] const Bytes&         bytes() const noexcept { return bytes_; }
    [[nodiscard]] const std::uint8_t*  data()  const noexcept { return bytes_.data(); }
    [[nodiscard]] std::string          str()   const;

    friend constexpr bool operator==(const MacAddress&, const MacAddress&) noexcept = default;

private:
    Bytes bytes_{};
};

std::ostream& operator<<(std::ostream& os, const MacAddress& mac);

// An IPv4 network: a base address plus a prefix length. The constructor masks
// off host bits, so a Prefix is always canonical by construction and
// "10.0.0.7/24" and "10.0.0.0/24" are the same object.
class Prefix {
public:
    constexpr Prefix() noexcept = default;
    Prefix(Ipv4Address network, std::uint8_t length) noexcept;

    static std::optional<Prefix> parse(std::string_view text);   // "a.b.c.d/len"

    [[nodiscard]] Ipv4Address  network() const noexcept { return network_; }
    [[nodiscard]] std::uint8_t length()  const noexcept { return length_; }
    [[nodiscard]] bool         contains(Ipv4Address addr) const noexcept;
    [[nodiscard]] std::string  str() const;

    friend constexpr bool operator==(const Prefix&, const Prefix&) noexcept = default;

private:
    Ipv4Address  network_{};
    std::uint8_t length_{0};
};

std::ostream& operator<<(std::ostream& os, const Prefix& prefix);

}  // namespace sdr
