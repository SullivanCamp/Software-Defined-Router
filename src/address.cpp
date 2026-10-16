#include "sdr/address.hpp"

#include <arpa/inet.h>

#include <charconv>
#include <cstdio>
#include <cstring>

namespace sdr {

std::optional<Ipv4Address> Ipv4Address::parse(std::string_view text) {
    // inet_pton needs NUL termination, and string_view does not promise it.
    if (text.empty() || text.size() > 15) {
        return std::nullopt;
    }
    char buffer[16];
    std::memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = '\0';

    in_addr addr{};
    if (::inet_pton(AF_INET, buffer, &addr) != 1) {
        return std::nullopt;
    }
    return Ipv4Address{::ntohl(addr.s_addr)};
}

Ipv4Address Ipv4Address::from_network(std::uint32_t network_order) noexcept {
    return Ipv4Address{::ntohl(network_order)};
}

std::uint32_t Ipv4Address::network() const noexcept { return ::htonl(value_); }

std::string Ipv4Address::str() const {
    char buffer[INET_ADDRSTRLEN];
    const std::uint32_t be = network();
    if (::inet_ntop(AF_INET, &be, buffer, sizeof buffer) == nullptr) {
        return "?";
    }
    return buffer;
}

std::ostream& operator<<(std::ostream& os, Ipv4Address addr) {
    return os << addr.str();
}

std::optional<MacAddress> MacAddress::parse(std::string_view text) {
    Bytes bytes{};
    std::size_t pos = 0;

    for (std::size_t i = 0; i < kSize; ++i) {
        if (pos + 2 > text.size()) {
            return std::nullopt;
        }
        unsigned value = 0;
        const char* first = text.data() + pos;
        const auto result = std::from_chars(first, first + 2, value, 16);
        if (result.ec != std::errc{} || result.ptr != first + 2) {
            return std::nullopt;
        }
        bytes[i] = static_cast<std::uint8_t>(value);
        pos += 2;

        if (i + 1 < kSize) {
            if (pos >= text.size() || (text[pos] != ':' && text[pos] != '-')) {
                return std::nullopt;
            }
            ++pos;
        }
    }

    if (pos != text.size()) {
        return std::nullopt;   // trailing junk
    }
    return MacAddress{bytes};
}

std::string MacAddress::str() const {
    char buffer[18];
    std::snprintf(buffer, sizeof buffer, "%02x:%02x:%02x:%02x:%02x:%02x",
                  bytes_[0], bytes_[1], bytes_[2], bytes_[3], bytes_[4], bytes_[5]);
    return buffer;
}

std::ostream& operator<<(std::ostream& os, const MacAddress& mac) {
    return os << mac.str();
}

namespace {
constexpr std::uint32_t mask_for(std::uint8_t length) noexcept {
    return length == 0 ? 0u : (0xFFFFFFFFu << (32 - length));
}
}  // namespace

Prefix::Prefix(Ipv4Address network, std::uint8_t length) noexcept
    : network_(Ipv4Address{network.host() & mask_for(length)}), length_(length) {}

std::optional<Prefix> Prefix::parse(std::string_view text) {
    const auto slash = text.find('/');
    if (slash == std::string_view::npos) {
        return std::nullopt;
    }

    const auto address = Ipv4Address::parse(text.substr(0, slash));
    if (!address) {
        return std::nullopt;
    }

    const std::string_view length_text = text.substr(slash + 1);
    if (length_text.empty()) {
        return std::nullopt;
    }

    unsigned length = 0;
    const auto result = std::from_chars(
        length_text.data(), length_text.data() + length_text.size(), length);
    if (result.ec != std::errc{} ||
        result.ptr != length_text.data() + length_text.size() || length > 32) {
        return std::nullopt;
    }

    return Prefix{*address, static_cast<std::uint8_t>(length)};
}

bool Prefix::contains(Ipv4Address addr) const noexcept {
    return (addr.host() & mask_for(length_)) == network_.host();
}

std::string Prefix::str() const {
    return network_.str() + "/" + std::to_string(length_);
}

std::ostream& operator<<(std::ostream& os, const Prefix& prefix) {
    return os << prefix.str();
}

}  // namespace sdr
