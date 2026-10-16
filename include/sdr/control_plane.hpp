// Line-oriented control protocol over TCP.
//
// Single-threaded poll() loop. It owns no data-plane state directly and mutates
// the tables only through Published<T>::update, so a slow or hostile client can
// never stall forwarding.
//
// Grammar:
//   ADD_ROUTE <a.b.c.d/len> <iface> [VIA <a.b.c.d>]
//   DEL_ROUTE <a.b.c.d/len>
//   ADD_NEIGH <a.b.c.d> <aa:bb:cc:dd:ee:ff>
//   DEL_NEIGH <a.b.c.d>
//   SHOW ROUTES | SHOW NEIGH | SHOW IFACES | SHOW STATS
//   PING | HELP | QUIT
//
// Every reply ends with a line beginning "OK" or "ERR", which lets a client
// read a response of unknown length without guessing a timeout.
#pragma once

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sdr/router.hpp"

namespace sdr {

struct CommandResult {
    std::string reply;
    bool        close_connection{false};
};

class ControlPlane {
public:
    explicit ControlPlane(Router& router);

    // Executes a single command line.
    [[nodiscard]] CommandResult execute(std::string_view line);

    // Applies a file of command lines. Returns the number of failing lines, or
    // -1 if the file could not be read.
    int load_file(const std::string& path);

    // Serves until stop_requested. Returns false if the listener could not be
    // created.
    bool serve(std::string_view bind_address, std::uint16_t port,
               std::stop_token token);

private:
    using Args    = std::vector<std::string_view>;
    using Handler = std::function<CommandResult(ControlPlane&, const Args&)>;

    CommandResult cmd_add_route(const Args& args);
    CommandResult cmd_del_route(const Args& args);
    CommandResult cmd_add_neighbor(const Args& args);
    CommandResult cmd_del_neighbor(const Args& args);
    CommandResult cmd_show(const Args& args);
    CommandResult cmd_ping(const Args& args);
    CommandResult cmd_help(const Args& args);
    CommandResult cmd_quit(const Args& args);

    std::string show_routes()     const;
    std::string show_neighbors()  const;
    std::string show_interfaces() const;
    std::string show_stats()      const;

    Router&                                        router_;
    std::unordered_map<std::string_view, Handler>  handlers_;
};

}  // namespace sdr
