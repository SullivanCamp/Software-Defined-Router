#include "sdr/control_plane.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "sdr/file_descriptor.hpp"
#include "sdr/log.hpp"

namespace sdr {
namespace {

constexpr std::string_view kHelpText =
    "commands:\n"
    "  ADD_ROUTE <a.b.c.d/len> <iface> [VIA <a.b.c.d>]\n"
    "  DEL_ROUTE <a.b.c.d/len>\n"
    "  ADD_NEIGH <a.b.c.d> <aa:bb:cc:dd:ee:ff>\n"
    "  DEL_NEIGH <a.b.c.d>\n"
    "  SHOW ROUTES | SHOW NEIGH | SHOW IFACES | SHOW STATS\n"
    "  PING | HELP | QUIT\n";

std::vector<std::string_view> tokenize(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t pos = 0;

    while (pos < line.size()) {
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
            ++pos;
        }
        const std::size_t start = pos;
        while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos]))) {
            ++pos;
        }
        if (pos > start) {
            tokens.push_back(line.substr(start, pos - start));
        }
    }
    return tokens;
}

std::string upper(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

CommandResult ok(std::string message) { return CommandResult{std::move(message), false}; }
CommandResult error(std::string_view message) {
    return CommandResult{"ERR " + std::string(message) + "\n", false};
}

}  // namespace

ControlPlane::ControlPlane(Router& router) : router_(router) {
    // A dispatch table rather than an if-else chain: adding a command is one
    // entry, and every handler has the same shape.
    handlers_ = {
        {"ADD_ROUTE", [](ControlPlane& self, const Args& a) { return self.cmd_add_route(a); }},
        {"DEL_ROUTE", [](ControlPlane& self, const Args& a) { return self.cmd_del_route(a); }},
        {"ADD_NEIGH", [](ControlPlane& self, const Args& a) { return self.cmd_add_neighbor(a); }},
        {"DEL_NEIGH", [](ControlPlane& self, const Args& a) { return self.cmd_del_neighbor(a); }},
        {"SHOW",      [](ControlPlane& self, const Args& a) { return self.cmd_show(a); }},
        {"PING",      [](ControlPlane& self, const Args& a) { return self.cmd_ping(a); }},
        {"HELP",      [](ControlPlane& self, const Args& a) { return self.cmd_help(a); }},
        {"QUIT",      [](ControlPlane& self, const Args& a) { return self.cmd_quit(a); }},
        {"EXIT",      [](ControlPlane& self, const Args& a) { return self.cmd_quit(a); }},
    };
}

CommandResult ControlPlane::execute(std::string_view line) {
    const Args tokens = tokenize(line);
    if (tokens.empty()) {
        return ok("OK\n");
    }

    const std::string verb = upper(tokens[0]);
    const auto        it   = handlers_.find(verb);
    if (it == handlers_.end()) {
        return error("unknown command '" + verb + "' (try HELP)");
    }
    return it->second(*this, tokens);
}

CommandResult ControlPlane::cmd_add_route(const Args& args) {
    if (args.size() < 3) {
        return error("usage: ADD_ROUTE <a.b.c.d/len> <iface> [VIA <a.b.c.d>]");
    }

    const auto prefix = Prefix::parse(args[1]);
    if (!prefix) {
        return error("malformed prefix '" + std::string(args[1]) + "'");
    }

    const auto port = router_.ports().find(args[2]);
    if (!port) {
        return error("unknown interface '" + std::string(args[2]) + "'");
    }

    std::optional<Ipv4Address> next_hop;
    if (args.size() > 3) {
        if (args.size() != 5 || upper(args[3]) != "VIA") {
            return error("expected 'VIA <a.b.c.d>'");
        }
        next_hop = Ipv4Address::parse(args[4]);
        if (!next_hop) {
            return error("malformed next hop '" + std::string(args[4]) + "'");
        }
    }

    const Route route{*prefix, *port, next_hop};
    if (!router_.routes().update([&route](RouteTable& table) {
            table.insert(route);
            return true;
        })) {
        return error("route install failed");
    }

    log_info("route installed: ", *prefix, " -> ", args[2]);
    return ok("OK route " + prefix->str() + " -> " + std::string(args[2]) + "\n");
}

CommandResult ControlPlane::cmd_del_route(const Args& args) {
    if (args.size() != 2) {
        return error("usage: DEL_ROUTE <a.b.c.d/len>");
    }
    const auto prefix = Prefix::parse(args[1]);
    if (!prefix) {
        return error("malformed prefix '" + std::string(args[1]) + "'");
    }

    // update() returning false here means the mutation aborted, so nothing was
    // published and no grace period was paid.
    if (!router_.routes().update(
            [&prefix](RouteTable& table) { return table.erase(*prefix); })) {
        return error("no such route");
    }

    log_info("route removed: ", *prefix);
    return ok("OK removed " + prefix->str() + "\n");
}

CommandResult ControlPlane::cmd_add_neighbor(const Args& args) {
    if (args.size() != 3) {
        return error("usage: ADD_NEIGH <a.b.c.d> <aa:bb:cc:dd:ee:ff>");
    }
    const auto ip  = Ipv4Address::parse(args[1]);
    const auto mac = MacAddress::parse(args[2]);
    if (!ip || !mac) {
        return error("usage: ADD_NEIGH <a.b.c.d> <aa:bb:cc:dd:ee:ff>");
    }

    if (!router_.neighbors().update([&](NeighborTable& table) {
            table.insert(*ip, *mac);
            return true;
        })) {
        return error("neighbor table full");
    }
    return ok("OK neighbor " + ip->str() + "\n");
}

CommandResult ControlPlane::cmd_del_neighbor(const Args& args) {
    if (args.size() != 2) {
        return error("usage: DEL_NEIGH <a.b.c.d>");
    }
    const auto ip = Ipv4Address::parse(args[1]);
    if (!ip) {
        return error("malformed address '" + std::string(args[1]) + "'");
    }
    if (!router_.neighbors().update(
            [&ip](NeighborTable& table) { return table.erase(*ip); })) {
        return error("no such neighbor");
    }
    return ok("OK removed " + ip->str() + "\n");
}

CommandResult ControlPlane::cmd_show(const Args& args) {
    if (args.size() != 2) {
        return error("usage: SHOW ROUTES|NEIGH|IFACES|STATS");
    }
    const std::string what = upper(args[1]);
    if (what == "ROUTES") return ok(show_routes());
    if (what == "NEIGH")  return ok(show_neighbors());
    if (what == "IFACES") return ok(show_interfaces());
    if (what == "STATS")  return ok(show_stats());
    return error("unknown SHOW target '" + what + "'");
}

CommandResult ControlPlane::cmd_ping(const Args&)  { return ok("OK pong\n"); }
CommandResult ControlPlane::cmd_help(const Args&)  { return ok(std::string(kHelpText) + "OK\n"); }
CommandResult ControlPlane::cmd_quit(const Args&)  { return CommandResult{"OK bye\n", true}; }

std::string ControlPlane::show_routes() const {
    const auto routes = router_.routes().inspect(
        [](const RouteTable& table) { return table.dump(); });

    std::ostringstream out;
    out << std::left << std::setw(20) << "PREFIX" << std::setw(10) << "IFACE"
        << "NEXTHOP" << '\n';
    for (const Route& route : routes) {
        const Interface* port = router_.ports().at(route.out_port);
        out << std::left << std::setw(20) << route.prefix.str() << std::setw(10)
            << (port != nullptr ? port->name() : "?")
            << (route.next_hop ? route.next_hop->str() : "direct") << '\n';
    }
    out << "OK " << routes.size() << " route(s)\n";
    return out.str();
}

std::string ControlPlane::show_neighbors() const {
    const auto entries = router_.neighbors().inspect(
        [](const NeighborTable& table) { return table.entries(); });

    std::ostringstream out;
    out << std::left << std::setw(16) << "IP" << "MAC" << '\n';
    for (const NeighborEntry& entry : entries) {
        out << std::left << std::setw(16) << entry.ip.str() << entry.mac.str() << '\n';
    }
    out << "OK " << entries.size() << " neighbor(s)\n";
    return out.str();
}

std::string ControlPlane::show_interfaces() const {
    std::ostringstream out;
    out << std::left << std::setw(6) << "PORT" << std::setw(10) << "NAME"
        << std::setw(9) << "IFINDEX" << "MAC" << '\n';
    for (const auto& port : router_.ports()) {
        out << std::left << std::setw(6) << port->port() << std::setw(10)
            << port->name() << std::setw(9) << port->index() << port->mac().str()
            << '\n';
    }
    out << "OK " << router_.ports().size() << " interface(s)\n";
    return out.str();
}

std::string ControlPlane::show_stats() const {
    std::ostringstream out;
    for (const auto& port : router_.ports()) {
        const PortCounters& c = port->counters();
        const auto value = [](const std::atomic<std::uint64_t>& counter) {
            return counter.load(std::memory_order_relaxed);
        };

        out << port->name() << ":\n"
            << "  rx  packets=" << value(c.rx_packets)
            << " bytes=" << value(c.rx_bytes) << '\n'
            << "  tx  packets=" << value(c.tx_packets)
            << " bytes=" << value(c.tx_bytes)
            << " errors=" << value(c.tx_errors) << '\n'
            << "  fwd packets=" << value(c.forwarded) << '\n'
            << "  drop not_for_us=" << value(c.drop_not_for_us)
            << " non_ipv4=" << value(c.drop_non_ipv4)
            << " malformed=" << value(c.drop_malformed)
            << " checksum=" << value(c.drop_checksum) << '\n'
            << "  drop ttl=" << value(c.drop_ttl)
            << " no_route=" << value(c.drop_no_route)
            << " no_neighbor=" << value(c.drop_no_neighbor)
            << " hairpin=" << value(c.drop_hairpin) << '\n';
    }
    out << "OK\n";
    return out.str();
}

int ControlPlane::load_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        log_error("cannot open config ", path);
        return -1;
    }

    std::string line;
    int failures = 0;
    int line_number = 0;

    while (std::getline(file, line)) {
        ++line_number;
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        const CommandResult result = execute(line);
        if (result.reply.starts_with("ERR")) {
            log_error(path, ":", line_number, ": ", result.reply);
            ++failures;
        }
    }

    log_info("loaded ", path, " (", failures, " error(s))");
    return failures;
}

namespace {

struct Client {
    FileDescriptor socket;
    std::string    buffer;

    [[nodiscard]] bool active() const noexcept { return socket.valid(); }
};

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t written = ::write(fd, data.data(), data.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

}  // namespace

bool ControlPlane::serve(std::string_view bind_address, std::uint16_t port,
                         std::stop_token token) {
    FileDescriptor listener(::socket(AF_INET, SOCK_STREAM, 0));
    if (!listener) {
        log_error("control socket: ", std::strerror(errno));
        return false;
    }

    int reuse = 1;
    ::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port   = ::htons(port);

    const std::string bind_str(bind_address);
    if (::inet_pton(AF_INET, bind_str.c_str(), &address.sin_addr) != 1) {
        log_error("bad control bind address '", bind_str, "'");
        return false;
    }

    if (::bind(listener.get(), reinterpret_cast<sockaddr*>(&address),
               sizeof address) < 0 ||
        ::listen(listener.get(), 8) < 0) {
        log_error("control bind/listen on ", bind_str, ":", port, ": ",
                  std::strerror(errno));
        return false;
    }

    log_info("control plane listening on ", bind_str, ":", port);

    std::array<Client, kMaxControlClients> clients{};

    while (!token.stop_requested()) {
        std::vector<pollfd>      poll_fds;
        std::vector<std::size_t> slot_of;
        poll_fds.push_back(pollfd{listener.get(), POLLIN, 0});
        slot_of.push_back(kMaxControlClients);

        for (std::size_t i = 0; i < clients.size(); ++i) {
            if (clients[i].active()) {
                poll_fds.push_back(pollfd{clients[i].socket.get(), POLLIN, 0});
                slot_of.push_back(i);
            }
        }

        const int ready = ::poll(poll_fds.data(), poll_fds.size(), 200);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_error("control poll: ", std::strerror(errno));
            break;
        }
        if (ready == 0) {
            continue;
        }

        for (std::size_t i = 1; i < poll_fds.size(); ++i) {
            if (poll_fds[i].revents == 0) {
                continue;
            }
            Client& client = clients[slot_of[i]];

            if (poll_fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                client.socket.reset();
                client.buffer.clear();
                continue;
            }

            std::array<char, 1024> chunk{};
            const ssize_t received =
                ::read(client.socket.get(), chunk.data(), chunk.size());
            if (received <= 0) {
                if (received < 0 && (errno == EINTR || errno == EAGAIN)) {
                    continue;
                }
                client.socket.reset();
                client.buffer.clear();
                continue;
            }
            client.buffer.append(chunk.data(), static_cast<std::size_t>(received));

            if (client.buffer.size() > kMaxControlLine &&
                client.buffer.find('\n') == std::string::npos) {
                write_all(client.socket.get(), "ERR line too long\n");
                client.socket.reset();
                client.buffer.clear();
                continue;
            }

            bool keep_open = true;
            std::size_t newline;
            while (keep_open &&
                   (newline = client.buffer.find('\n')) != std::string::npos) {
                const std::string line = client.buffer.substr(0, newline);
                client.buffer.erase(0, newline + 1);

                const CommandResult result = execute(line);
                if (!write_all(client.socket.get(), result.reply) ||
                    result.close_connection) {
                    keep_open = false;
                }
            }
            if (!keep_open) {
                client.socket.reset();
                client.buffer.clear();
            }
        }

        if (poll_fds[0].revents & POLLIN) {
            FileDescriptor incoming(::accept(listener.get(), nullptr, nullptr));
            if (!incoming) {
                continue;
            }

            const auto free_slot = std::find_if(
                clients.begin(), clients.end(),
                [](const Client& c) { return !c.active(); });

            if (free_slot == clients.end()) {
                write_all(incoming.get(), "ERR too many clients\n");
            } else {
                write_all(incoming.get(),
                          "OK sdrouter control plane ready (HELP for commands)\n");
                free_slot->socket = std::move(incoming);
                free_slot->buffer.clear();
            }
        }
    }

    log_info("control plane stopped");
    return true;
}

}  // namespace sdr
