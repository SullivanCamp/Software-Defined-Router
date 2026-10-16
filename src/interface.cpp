#include "sdr/interface.hpp"

#include <arpa/inet.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "sdr/log.hpp"
#include "sdr/packet.hpp"

#ifndef ETH_P_ALL
#define ETH_P_ALL 0x0003
#endif

namespace sdr {

Interface::Interface(std::string name, int if_index, MacAddress mac, PortId port,
                     FileDescriptor socket)
    : name_(std::move(name)),
      if_index_(if_index),
      mac_(mac),
      port_(port),
      socket_(std::move(socket)) {}

std::unique_ptr<Interface> Interface::open(std::string_view name, PortId port) {
    const std::string name_str(name);

    const unsigned if_index = ::if_nametoindex(name_str.c_str());
    if (if_index == 0) {
        log_error("if_nametoindex(", name_str, "): ", std::strerror(errno));
        return nullptr;
    }

    // Wrapped immediately: every failure path below closes it automatically.
    FileDescriptor socket_fd(::socket(AF_PACKET, SOCK_RAW, ::htons(ETH_P_ALL)));
    if (!socket_fd) {
        log_error("socket(AF_PACKET): ", std::strerror(errno),
                  errno == EPERM ? " (needs root or CAP_NET_RAW)" : "");
        return nullptr;
    }

    ifreq request{};
    std::snprintf(request.ifr_name, IFNAMSIZ, "%s", name_str.c_str());
    if (::ioctl(socket_fd.get(), SIOCGIFHWADDR, &request) < 0) {
        log_error("SIOCGIFHWADDR(", name_str, "): ", std::strerror(errno));
        return nullptr;
    }

    MacAddress::Bytes mac_bytes{};
    std::memcpy(mac_bytes.data(), request.ifr_hwaddr.sa_data, MacAddress::kSize);
    const MacAddress mac{mac_bytes};

    // Bind before enabling promiscuous mode, so only this link's traffic arrives.
    sockaddr_ll address{};
    address.sll_family   = AF_PACKET;
    address.sll_protocol = ::htons(ETH_P_ALL);
    address.sll_ifindex  = static_cast<int>(if_index);
    if (::bind(socket_fd.get(), reinterpret_cast<sockaddr*>(&address),
               sizeof address) < 0) {
        log_error("bind(", name_str, "): ", std::strerror(errno));
        return nullptr;
    }

    packet_mreq membership{};
    membership.mr_ifindex = static_cast<int>(if_index);
    membership.mr_type    = PACKET_MR_PROMISC;
    if (::setsockopt(socket_fd.get(), SOL_PACKET, PACKET_ADD_MEMBERSHIP,
                     &membership, sizeof membership) < 0) {
        log_warn("promiscuous mode on ", name_str, " failed: ",
                 std::strerror(errno), " (transit traffic may be missed)");
    }

    int receive_buffer = 4 * 1024 * 1024;
    ::setsockopt(socket_fd.get(), SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                 sizeof receive_buffer);

    log_info("interface ", name_str, " registered (ifindex ", if_index, ", mac ",
             mac, ") as port ", port);

    // Private constructor, so make_unique is not available here.
    return std::unique_ptr<Interface>(new Interface(
        name_str, static_cast<int>(if_index), mac, port, std::move(socket_fd)));
}

bool Interface::send(std::span<const std::uint8_t> frame,
                     const MacAddress& destination) noexcept {
    sockaddr_ll address{};
    address.sll_family   = AF_PACKET;
    address.sll_protocol = ::htons(kEtherTypeIpv4);
    address.sll_ifindex  = if_index_;
    address.sll_halen    = MacAddress::kSize;
    std::memcpy(address.sll_addr, destination.data(), MacAddress::kSize);

    ssize_t written = 0;
    do {
        written = ::sendto(socket_.get(), frame.data(), frame.size(), 0,
                           reinterpret_cast<sockaddr*>(&address), sizeof address);
    } while (written < 0 && errno == EINTR);

    if (written < 0) {
        PortCounters::bump(counters_.tx_errors);
        log_debug("tx on ", name_, " failed: ", std::strerror(errno));
        return false;
    }

    PortCounters::bump(counters_.tx_packets);
    PortCounters::add(counters_.tx_bytes, static_cast<std::uint64_t>(written));
    return true;
}

bool PortTable::add(std::string_view name) {
    if (ports_.size() >= kMaxInterfaces) {
        log_error("interface limit (", kMaxInterfaces, ") reached");
        return false;
    }
    if (find(name).has_value()) {
        log_error("interface ", name, " already registered");
        return false;
    }

    auto interface = Interface::open(name, ports_.size());
    if (!interface) {
        return false;
    }
    ports_.push_back(std::move(interface));
    return true;
}

Interface* PortTable::at(PortId id) noexcept {
    return id < ports_.size() ? ports_[id].get() : nullptr;
}

const Interface* PortTable::at(PortId id) const noexcept {
    return id < ports_.size() ? ports_[id].get() : nullptr;
}

std::optional<PortId> PortTable::find(std::string_view name) const noexcept {
    const auto it = std::find_if(
        ports_.begin(), ports_.end(),
        [name](const std::unique_ptr<Interface>& p) { return p->name() == name; });
    if (it == ports_.end()) {
        return std::nullopt;
    }
    return static_cast<PortId>(std::distance(ports_.begin(), it));
}

}  // namespace sdr
