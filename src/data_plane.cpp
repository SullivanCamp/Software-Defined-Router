#include "sdr/data_plane.hpp"

#include <arpa/inet.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <cstring>

#include "sdr/log.hpp"
#include "sdr/packet.hpp"

namespace sdr {

bool DataPlane::start() {
    if (router_.ports().empty()) {
        log_error("no interfaces registered");
        return false;
    }

    workers_.reserve(router_.ports().size());
    for (PortId port = 0; port < router_.ports().size(); ++port) {
        workers_.emplace_back([this, port](std::stop_token token) {
            run_port(std::move(token), port);
        });
    }
    return true;
}

void DataPlane::stop() noexcept {
    for (auto& worker : workers_) {
        worker.request_stop();
    }
    workers_.clear();   // jthread joins on destruction
}

void DataPlane::run_port(std::stop_token token, PortId port) {
    Interface* interface = router_.ports().at(port);
    if (interface == nullptr) {
        return;
    }

    Qsbr::Reader reader;
    try {
        reader = router_.qsbr().register_reader();
    } catch (const std::exception& error) {
        log_error(interface->name(), ": ", error.what());
        return;
    }

    log_info("data plane thread up on ", interface->name());

    std::array<std::uint8_t, kMaxFrameBytes> buffer{};
    pollfd poll_fd{interface->fd(), POLLIN, 0};

    while (!token.stop_requested()) {
        const int ready = ::poll(&poll_fd, 1, 200);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_error(interface->name(), ": poll: ", std::strerror(errno));
            break;
        }
        if (ready == 0) {
            continue;
        }

        sockaddr_ll from{};
        socklen_t   from_length = sizeof from;
        const ssize_t received =
            ::recvfrom(interface->fd(), buffer.data(), buffer.size(), MSG_TRUNC,
                       reinterpret_cast<sockaddr*>(&from), &from_length);
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            log_error(interface->name(), ": recvfrom: ", std::strerror(errno));
            break;
        }

        // AF_PACKET echoes our own transmissions back. Without this check every
        // forwarded frame would loop straight back through the FIB.
        if (from.sll_pkttype == PACKET_OUTGOING) {
            continue;
        }

        auto& counters = interface->counters();
        PortCounters::bump(counters.rx_packets);
        PortCounters::add(counters.rx_bytes, static_cast<std::uint64_t>(received));

        if (static_cast<std::size_t>(received) > buffer.size()) {
            PortCounters::bump(counters.drop_malformed);   // MSG_TRUNC told us
            continue;
        }

        // Promiscuous mode delivers frames addressed to other stations; a router
        // forwards only what was sent to its own MAC or to broadcast.
        if (from.sll_pkttype == PACKET_OTHERHOST) {
            PortCounters::bump(counters.drop_not_for_us);
            continue;
        }

        forward(*interface, reader,
                std::span<std::uint8_t>(buffer.data(),
                                        static_cast<std::size_t>(received)));
    }

    log_info("data plane thread on ", interface->name(), " exiting");
}

void DataPlane::forward(Interface& ingress, const Qsbr::Reader& reader,
                        std::span<std::uint8_t> frame) {
    auto& counters = ingress.counters();

    const ParseResult parsed = parse_frame(frame);
    if (const auto* error = std::get_if<PacketError>(&parsed)) {
        switch (*error) {
            case PacketError::NotIpv4:     PortCounters::bump(counters.drop_non_ipv4); break;
            case PacketError::BadChecksum: PortCounters::bump(counters.drop_checksum); break;
            case PacketError::TtlExpired:  PortCounters::bump(counters.drop_ttl);      break;
            default:                       PortCounters::bump(counters.drop_malformed);break;
        }
        log_debug(ingress.name(), ": dropped frame (", to_string(*error), ")");
        return;
    }

    const ParsedFrame& packet = std::get<ParsedFrame>(parsed);

    // lookup returns by value, so the route outlives the snapshot guard and
    // there is no way to accidentally read freed memory.
    std::optional<Route> route;
    {
        const auto snapshot = router_.routes().read(reader);
        route = snapshot->lookup(packet.destination);
    }

    if (!route) {
        PortCounters::bump(counters.drop_no_route);
        log_debug(ingress.name(), ": no route to ", packet.destination);
        return;
    }

    if (route->out_port == ingress.port()) {
        // Sending it back where it came from means the table or topology is
        // wrong. A real router would answer with an ICMP redirect.
        PortCounters::bump(counters.drop_hairpin);
        return;
    }

    Interface* egress = router_.ports().at(route->out_port);
    if (egress == nullptr) {
        PortCounters::bump(counters.drop_no_route);
        return;
    }

    const Ipv4Address next_hop = route->next_hop.value_or(packet.destination);

    std::optional<MacAddress> destination_mac;
    {
        const auto snapshot = router_.neighbors().read(reader);
        destination_mac = snapshot->lookup(next_hop);
    }

    if (!destination_mac) {
        PortCounters::bump(counters.drop_no_neighbor);
        log_debug(ingress.name(), ": no neighbor entry for ", next_hop);
        return;
    }

    decrement_ttl(*packet.ip);

    std::memcpy(packet.eth->dst, destination_mac->data(), MacAddress::kSize);
    std::memcpy(packet.eth->src, egress->mac().data(), MacAddress::kSize);

    // Forward exactly what IP claims is there, trimming Ethernet padding.
    const std::size_t length = std::min(
        frame.size(), kEthHeaderLen + static_cast<std::size_t>(::ntohs(packet.ip->total_length)));

    if (egress->send(frame.subspan(0, length), *destination_mac)) {
        PortCounters::bump(counters.forwarded);
        log_debug("fwd ", ingress.name(), " -> ", egress->name(), " : ",
                  packet.source, " -> ", packet.destination,
                  " ttl=", static_cast<unsigned>(packet.ip->ttl),
                  " len=", length);
    }
}

}  // namespace sdr
