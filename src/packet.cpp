#include "sdr/packet.hpp"

#include <arpa/inet.h>

#include <cstring>

namespace sdr {

std::string_view to_string(PacketError error) {
    switch (error) {
        case PacketError::Runt:            return "runt";
        case PacketError::NotIpv4:         return "not-ipv4";
        case PacketError::BadVersion:      return "bad-version";
        case PacketError::BadHeaderLength: return "bad-ihl";
        case PacketError::BadTotalLength:  return "bad-length";
        case PacketError::BadChecksum:     return "bad-checksum";
        case PacketError::TtlExpired:      return "ttl-expired";
    }
    return "unknown";
}

std::uint16_t ip_checksum(std::span<const std::uint8_t> data) noexcept {
    std::uint32_t sum = 0;
    std::size_t   i   = 0;

    for (; i + 1 < data.size(); i += 2) {
        std::uint16_t word = 0;
        std::memcpy(&word, data.data() + i, sizeof word);   // alignment-safe
        sum += word;
    }
    if (i < data.size()) {
        sum += data[i];
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return static_cast<std::uint16_t>(~sum);
}

namespace {

constexpr std::uint16_t ones_complement_add(std::uint16_t a, std::uint16_t b) noexcept {
    const std::uint32_t sum = static_cast<std::uint32_t>(a) + b;
    return static_cast<std::uint16_t>(sum + (sum >> 16));
}

constexpr std::uint16_t ones_complement_sub(std::uint16_t a, std::uint16_t b) noexcept {
    return ones_complement_add(a, static_cast<std::uint16_t>(~b));
}

}  // namespace

std::uint16_t checksum_replace(std::uint16_t checksum, std::uint16_t old_word,
                               std::uint16_t new_word) noexcept {
    // RFC 1624 eqn 3: HC' = ~(~HC + ~m + m')
    return static_cast<std::uint16_t>(
        ~ones_complement_add(
            ones_complement_sub(static_cast<std::uint16_t>(~checksum), old_word),
            new_word));
}

void decrement_ttl(Ipv4Header& ip) noexcept {
    // TTL and protocol form one 16-bit word at header offset 8. Patching the
    // checksum incrementally is constant work regardless of header length,
    // which is what real forwarding hardware does.
    const auto word_of = [](std::uint8_t ttl, std::uint8_t protocol) {
        return static_cast<std::uint16_t>(
            ::htons(static_cast<std::uint16_t>(ttl << 8 | protocol)));
    };

    const std::uint16_t old_word = word_of(ip.ttl, ip.protocol);
    --ip.ttl;
    const std::uint16_t new_word = word_of(ip.ttl, ip.protocol);

    ip.checksum = checksum_replace(ip.checksum, old_word, new_word);
}

ParseResult parse_frame(std::span<std::uint8_t> frame) noexcept {
    if (frame.size() < kEthHeaderLen) {
        return PacketError::Runt;
    }

    auto* eth = reinterpret_cast<EthHeader*>(frame.data());
    if (::ntohs(eth->ethertype) != kEtherTypeIpv4) {
        return PacketError::NotIpv4;
    }

    if (frame.size() < kEthHeaderLen + kIpv4MinHeaderLen) {
        return PacketError::Runt;
    }

    auto* ip = reinterpret_cast<Ipv4Header*>(frame.data() + kEthHeaderLen);
    if (ip->version() != 4) {
        return PacketError::BadVersion;
    }

    const std::size_t header_length = ip->header_length();
    if (header_length < kIpv4MinHeaderLen ||
        frame.size() < kEthHeaderLen + header_length) {
        return PacketError::BadHeaderLength;
    }

    // total_length may be shorter than the frame because of Ethernet padding,
    // but it must never claim more bytes than actually arrived.
    const std::size_t total_length = ::ntohs(ip->total_length);
    if (total_length < header_length ||
        kEthHeaderLen + total_length > frame.size()) {
        return PacketError::BadTotalLength;
    }

    const std::span<const std::uint8_t> header{
        reinterpret_cast<const std::uint8_t*>(ip), header_length};
    if (ip_checksum(header) != 0) {
        return PacketError::BadChecksum;
    }

    if (ip->ttl <= 1) {
        return PacketError::TtlExpired;
    }

    return ParsedFrame{eth, ip, Ipv4Address::from_network(ip->dst),
                       Ipv4Address::from_network(ip->src)};
}

}  // namespace sdr
