// sdrouter - a userspace software-defined router.
//
//   control plane                 data plane
//   -------------                 ----------
//   TCP command loop        -->   one std::jthread per port
//   Published<T>::update          Published<T>::read (lock-free, QSBR)
#include <getopt.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "sdr/control_plane.hpp"
#include "sdr/data_plane.hpp"
#include "sdr/log.hpp"
#include "sdr/router.hpp"

namespace {

// A signal handler may only touch a volatile sig_atomic_t and write to a pipe.
// The pipe is what lets the control loop wake immediately instead of waiting
// out its poll timeout.
volatile sig_atomic_t g_signal_received = 0;
int                   g_wakeup_fd       = -1;

extern "C" void handle_signal(int) {
    g_signal_received = 1;
    if (g_wakeup_fd >= 0) {
        const char byte = 'x';
        ssize_t ignored = ::write(g_wakeup_fd, &byte, 1);
        static_cast<void>(ignored);
    }
}

void install_signal_handlers() {
    struct sigaction action {};
    sigemptyset(&action.sa_mask);
    action.sa_flags   = 0;             // no SA_RESTART: we want EINTR
    action.sa_handler = handle_signal;
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);

    // A control client vanishing mid-write must not kill the router.
    action.sa_handler = SIG_IGN;
    ::sigaction(SIGPIPE, &action, nullptr);
}

void usage(const char* program) {
    std::cerr
        << "usage: " << program << " -i <iface> [-i <iface> ...] [options]\n\n"
        << "  -i, --iface <name>   attach an interface (repeatable, max "
        << sdr::kMaxInterfaces << ")\n"
        << "  -f, --config <path>  apply control commands from a file at startup\n"
        << "  -a, --bind <addr>    control plane bind address (default 127.0.0.1)\n"
        << "  -p, --port <port>    control plane TCP port (default 5555)\n"
        << "  -v, --verbose        enable per-packet debug logging\n"
        << "  -h, --help           this message\n\n"
        << "requires CAP_NET_RAW (run as root, or:\n"
        << "  setcap cap_net_raw,cap_net_admin=eip ./sdrouter)\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> interface_names;
    std::string              config_path;
    std::string              bind_address = "127.0.0.1";
    long                     port         = 5555;
    bool                     verbose      = false;

    static const option long_options[] = {
        {"iface",   required_argument, nullptr, 'i'},
        {"config",  required_argument, nullptr, 'f'},
        {"bind",    required_argument, nullptr, 'a'},
        {"port",    required_argument, nullptr, 'p'},
        {"verbose", no_argument,       nullptr, 'v'},
        {"help",    no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int option = 0;
    while ((option = ::getopt_long(argc, argv, "i:f:a:p:vh", long_options,
                                   nullptr)) != -1) {
        switch (option) {
            case 'i': interface_names.emplace_back(optarg); break;
            case 'f': config_path  = optarg;                break;
            case 'a': bind_address = optarg;                break;
            case 'p': port = std::strtol(optarg, nullptr, 10); break;
            case 'v': verbose = true;                       break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    if (interface_names.empty()) {
        usage(argv[0]);
        return 1;
    }
    if (port <= 0 || port > 65535) {
        std::cerr << "port out of range\n";
        return 1;
    }

    sdr::set_log_level(verbose ? sdr::LogLevel::Debug : sdr::LogLevel::Info);
    install_signal_handlers();

    sdr::Router router;
    for (const std::string& name : interface_names) {
        if (!router.ports().add(name)) {
            sdr::log_error("failed to attach ", name, ", aborting");
            return 1;
        }
    }

    sdr::ControlPlane control(router);

    // Interfaces must exist before the config runs: ADD_ROUTE resolves names.
    if (!config_path.empty() && control.load_file(config_path) < 0) {
        return 1;
    }

    sdr::DataPlane data_plane(router);
    if (!data_plane.start()) {
        return 1;
    }

    const std::size_t route_count = router.routes().inspect(
        [](const sdr::RouteTable& table) { return table.size(); });
    sdr::log_info("sdrouter running with ", router.ports().size(),
                  " interface(s) and ", route_count, " route(s)");

    // Watches for a signal and asks the control loop to stop. Cheaper than
    // threading a self-pipe through the poll set, and just as prompt.
    std::stop_source stop_source;
    std::jthread signal_watcher([&stop_source](std::stop_token token) {
        while (!token.stop_requested()) {
            if (g_signal_received != 0) {
                stop_source.request_stop();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    control.serve(bind_address, static_cast<std::uint16_t>(port),
                  stop_source.get_token());

    signal_watcher.request_stop();
    data_plane.stop();          // also runs from the destructor
    sdr::log_info("shutdown complete");
    return 0;
}
