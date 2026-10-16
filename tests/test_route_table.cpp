// Unit tests for value types, the LPM trie, checksums, and frame validation.
// No root and no network required.
#include <arpa/inet.h>

#include <cstring>

#include "sdr/address.hpp"
#include "sdr/neighbor_table.hpp"
#include "sdr/packet.hpp"
#include "sdr/route_table.hpp"
#include "test_support.hpp"

using namespace sdr;

namespace {

Ipv4Address ip(std::string_view text) { return *Ipv4Address::parse(text); }
Prefix      pfx(std::string_view text) { return *Prefix::parse(text); }

Route route_to(std::string_view prefix, PortId port,
               std::optional<Ipv4Address> next_hop = std::nullopt) {
    return Route{pfx(prefix), port, next_hop};
}

void test_address_types() {
    test::section("address value types");

    CHECK(Ipv4Address::parse("10.0.0.1").has_value());
    CHECK(!Ipv4Address::parse("999.0.0.1").has_value());
    CHECK(!Ipv4Address::parse("").has_value());
    CHECK(!Ipv4Address::parse("not-an-address").has_value());
    CHECK_EQ(ip("192.168.0.1").str(), std::string("192.168.0.1"));

    // Host and network order are distinct and must not be confused.
    const Ipv4Address addr = ip("1.2.3.4");
    CHECK_EQ(addr.host(), 0x01020304u);
    CHECK(Ipv4Address::from_network(addr.network()) == addr);

    CHECK(MacAddress::parse("aa:bb:cc:dd:ee:ff").has_value());
    CHECK(MacAddress::parse("AA-BB-CC-DD-EE-FF").has_value());
    CHECK(!MacAddress::parse("aa:bb:cc:dd:ee").has_value());
    CHECK(!MacAddress::parse("aa:bb:cc:dd:ee:ff:00").has_value());
    CHECK(!MacAddress::parse("zz:bb:cc:dd:ee:ff").has_value());
    CHECK_EQ(MacAddress::parse("aa:bb:cc:dd:ee:ff")->str(),
             std::string("aa:bb:cc:dd:ee:ff"));

    // A Prefix is canonical by construction: host bits are masked off.
    CHECK(pfx("10.0.0.255/24") == pfx("10.0.0.0/24"));
    CHECK_EQ(pfx("10.1.2.200/24").str(), std::string("10.1.2.0/24"));
    CHECK(!Prefix::parse("10.0.0.0").has_value());
    CHECK(!Prefix::parse("10.0.0.0/33").has_value());
    CHECK(!Prefix::parse("10.0.0.0/-1").has_value());
    CHECK(!Prefix::parse("10.0.0.0/24x").has_value());
    CHECK(!Prefix::parse("10.0.0.0/").has_value());

    CHECK(pfx("10.0.0.0/8").contains(ip("10.9.9.9")));
    CHECK(!pfx("10.0.0.0/8").contains(ip("11.0.0.1")));
    CHECK(pfx("0.0.0.0/0").contains(ip("8.8.8.8")));
}

void test_longest_prefix_match() {
    test::section("longest prefix match");
    RouteTable table;

    CHECK(!table.lookup(ip("10.0.0.1")).has_value());

    table.insert(route_to("10.0.0.0/8", 1));
    table.insert(route_to("10.1.0.0/16", 2));
    table.insert(route_to("10.1.2.0/24", 3));
    table.insert(route_to("10.1.2.7/32", 4));

    CHECK_EQ(table.lookup(ip("10.9.9.9"))->out_port, PortId{1});
    CHECK_EQ(table.lookup(ip("10.1.9.9"))->out_port, PortId{2});
    CHECK_EQ(table.lookup(ip("10.1.2.9"))->out_port, PortId{3});
    CHECK_EQ(table.lookup(ip("10.1.2.7"))->out_port, PortId{4});
    CHECK(!table.lookup(ip("11.0.0.1")).has_value());
    CHECK_EQ(table.size(), std::size_t{4});
}

void test_default_route() {
    test::section("default route");
    RouteTable table;

    table.insert(route_to("0.0.0.0/0", 7, ip("192.168.1.1")));
    CHECK_EQ(table.lookup(ip("8.8.8.8"))->out_port, PortId{7});
    CHECK(table.lookup(ip("8.8.8.8"))->next_hop == ip("192.168.1.1"));

    table.insert(route_to("8.8.8.0/24", 9));
    CHECK_EQ(table.lookup(ip("8.8.8.8"))->out_port, PortId{9});
    CHECK(!table.lookup(ip("8.8.8.8"))->next_hop.has_value());
}

void test_insert_replace_and_erase() {
    test::section("insert, replace, erase, prune");
    RouteTable table;

    CHECK(table.insert(route_to("10.1.2.0/24", 1)));
    CHECK(!table.insert(route_to("10.1.2.0/24", 5)));      // replace, not add
    CHECK_EQ(table.size(), std::size_t{1});
    CHECK_EQ(table.lookup(ip("10.1.2.5"))->out_port, PortId{5});

    table.insert(route_to("10.0.0.0/8", 1));
    CHECK(table.erase(pfx("10.1.2.0/24")));
    CHECK(!table.erase(pfx("10.1.2.0/24")));               // double erase fails
    CHECK_EQ(table.lookup(ip("10.1.2.9"))->out_port, PortId{1});   // falls back
    CHECK_EQ(table.size(), std::size_t{1});

    CHECK(table.erase(pfx("10.0.0.0/8")));
    CHECK(!table.lookup(ip("10.1.2.9")).has_value());
    CHECK(table.empty());
}

void test_value_semantics() {
    test::section("value semantics (copy is a deep clone)");
    RouteTable original;
    original.insert(route_to("10.0.0.0/8", 1));

    RouteTable copy = original;                 // copy constructor
    copy.insert(route_to("20.0.0.0/8", 2));
    copy.erase(pfx("10.0.0.0/8"));

    CHECK(original.lookup(ip("10.0.0.1")).has_value());
    CHECK(!original.lookup(ip("20.0.0.1")).has_value());
    CHECK(!copy.lookup(ip("10.0.0.1")).has_value());
    CHECK(copy.lookup(ip("20.0.0.1")).has_value());

    RouteTable assigned;
    assigned = original;                        // copy assignment
    assigned.insert(route_to("30.0.0.0/8", 3));
    CHECK(!original.lookup(ip("30.0.0.1")).has_value());

    RouteTable moved = std::move(assigned);     // move constructor
    CHECK(moved.lookup(ip("30.0.0.1")).has_value());
}

void test_dump() {
    test::section("dump");
    RouteTable table;
    table.insert(route_to("10.0.0.0/8", 1));
    table.insert(route_to("20.0.0.0/24", 2));
    table.insert(route_to("0.0.0.0/0", 3));

    const auto routes = table.dump();
    CHECK_EQ(routes.size(), std::size_t{3});
    CHECK_EQ(routes.front().prefix.length(), 0);   // in-order: default first
}

void test_neighbor_table() {
    test::section("neighbor table");
    NeighborTable table;
    const MacAddress mac = *MacAddress::parse("aa:bb:cc:dd:ee:ff");
    const MacAddress other = *MacAddress::parse("11:22:33:44:55:66");

    CHECK(table.insert(ip("10.0.0.1"), mac));
    CHECK(!table.insert(ip("10.0.0.1"), other));      // replace, not add
    CHECK_EQ(table.size(), std::size_t{1});
    CHECK(table.lookup(ip("10.0.0.1")) == other);
    CHECK(!table.lookup(ip("10.0.0.2")).has_value());
    CHECK(table.erase(ip("10.0.0.1")));
    CHECK(!table.erase(ip("10.0.0.1")));
    CHECK(table.empty());
}

void test_checksums() {
    test::section("ip checksum");

    std::array<std::uint8_t, 20> header = {
        0x45, 0x00, 0x00, 0x54, 0xab, 0xcd, 0x40, 0x00,
        0x40, 0x01, 0x00, 0x00,
        10, 0, 1, 10,
        10, 0, 2, 10};

    const std::uint16_t checksum = ip_checksum(header);
    std::memcpy(header.data() + 10, &checksum, 2);
    CHECK_EQ(ip_checksum(header), std::uint16_t{0});

    header[15] ^= 0x01;
    CHECK(ip_checksum(header) != 0);
    header[15] ^= 0x01;

    test::section("incremental ttl update matches full recompute");
    bool all_match = true;
    for (int ttl = 2; ttl <= 255 && all_match; ++ttl) {
        auto incremental = header;
        incremental[8] = static_cast<std::uint8_t>(ttl);
        std::memset(incremental.data() + 10, 0, 2);
        const std::uint16_t base = ip_checksum(incremental);
        std::memcpy(incremental.data() + 10, &base, 2);

        auto recomputed = incremental;

        decrement_ttl(*reinterpret_cast<Ipv4Header*>(incremental.data()));

        recomputed[8] = static_cast<std::uint8_t>(ttl - 1);
        std::memset(recomputed.data() + 10, 0, 2);
        const std::uint16_t fresh = ip_checksum(recomputed);
        std::memcpy(recomputed.data() + 10, &fresh, 2);

        if (incremental != recomputed) {
            CHECK_MSG(false, "incremental checksum diverged at ttl ", ttl);
            all_match = false;
        }
        CHECK_EQ(ip_checksum(incremental), std::uint16_t{0});
    }
}

void test_frame_validation() {
    test::section("frame validation");

    std::array<std::uint8_t, 64> frame{};
    auto* eth = reinterpret_cast<EthHeader*>(frame.data());
    eth->ethertype = ::htons(kEtherTypeIpv4);

    auto* ipv4 = reinterpret_cast<Ipv4Header*>(frame.data() + kEthHeaderLen);
    ipv4->version_ihl  = 0x45;
    ipv4->total_length = ::htons(20);
    ipv4->ttl          = 64;
    ipv4->protocol     = 1;
    ipv4->src          = ip("10.0.1.10").network();
    ipv4->dst          = ip("10.0.2.10").network();

    const auto refresh_checksum = [&] {
        ipv4->checksum = 0;
        ipv4->checksum = ip_checksum({reinterpret_cast<std::uint8_t*>(ipv4), 20});
    };
    refresh_checksum();

    const std::size_t length = kEthHeaderLen + 20;
    const auto span_of = [&](std::size_t n) {
        return std::span<std::uint8_t>(frame.data(), n);
    };

    const auto error_of = [](const ParseResult& r) {
        return std::get<PacketError>(r);
    };

    // A valid frame parses, and the parsed view carries the right addresses.
    const auto good = parse_frame(span_of(length));
    CHECK(std::holds_alternative<ParsedFrame>(good));
    CHECK(std::get<ParsedFrame>(good).destination == ip("10.0.2.10"));
    CHECK(std::get<ParsedFrame>(good).source == ip("10.0.1.10"));

    CHECK(error_of(parse_frame(span_of(10))) == PacketError::Runt);

    eth->ethertype = ::htons(kEtherTypeArp);
    CHECK(error_of(parse_frame(span_of(length))) == PacketError::NotIpv4);
    eth->ethertype = ::htons(kEtherTypeIpv4);

    ipv4->version_ihl = 0x65;
    refresh_checksum();
    CHECK(error_of(parse_frame(span_of(length))) == PacketError::BadVersion);

    ipv4->version_ihl = 0x44;
    refresh_checksum();
    CHECK(error_of(parse_frame(span_of(length))) == PacketError::BadHeaderLength);

    ipv4->version_ihl = 0x45;
    ipv4->total_length = ::htons(1500);
    refresh_checksum();
    CHECK(error_of(parse_frame(span_of(length))) == PacketError::BadTotalLength);

    ipv4->total_length = ::htons(20);
    refresh_checksum();
    ipv4->checksum ^= ::htons(0x0001);
    CHECK(error_of(parse_frame(span_of(length))) == PacketError::BadChecksum);

    ipv4->ttl = 1;
    refresh_checksum();
    CHECK(error_of(parse_frame(span_of(length))) == PacketError::TtlExpired);

    // Ethernet padding: a 60-byte frame carrying a 20-byte packet is legal.
    ipv4->ttl = 64;
    refresh_checksum();
    CHECK(std::holds_alternative<ParsedFrame>(parse_frame(span_of(60))));
}

}  // namespace

int main() {
    test_address_types();
    test_longest_prefix_match();
    test_default_route();
    test_insert_replace_and_erase();
    test_value_semantics();
    test_dump();
    test_neighbor_table();
    test_checksums();
    test_frame_validation();
    return test::summary();
}
