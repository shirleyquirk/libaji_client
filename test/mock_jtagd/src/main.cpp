// mock-jtagd: run the mock JTAG server as a standalone process.
#include "mock_jtagd/server.hpp"

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <print>
#include <string>
#include <string_view>

namespace {

void usage(const char * argv0)
{
    std::println(stderr,
                 "Usage: {} [--port N] [--bind ADDR] [--protocol-version V] [--password PW]\n"
                 "          [--allow-remote] [--port-file FILE] [--trace]\n"
                 "Serves Scenario::default_board() until SIGINT/SIGTERM. --port 0 (default)\n"
                 "picks a free port; --port-file writes it out for test harnesses.",
                 argv0);
}

} // namespace

int main(int argc, char ** argv)
{
    mock_jtagd::Scenario scenario = mock_jtagd::Scenario::default_board();
    mock_jtagd::ServerOptions options;
    std::string port_file;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage(argv[0]);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--port")
            options.port = static_cast<std::uint16_t>(std::stoul(value()));
        else if (arg == "--bind")
            options.bind_address = value();
        else if (arg == "--protocol-version")
            scenario.version = static_cast<std::uint32_t>(std::stoul(value()));
        else if (arg == "--password")
            scenario.password = value();
        else if (arg == "--allow-remote")
            scenario.server_flags |= 1;
        else if (arg == "--port-file")
            port_file = value();
        else if (arg == "--trace")
            options.trace = true;
        else {
            usage(argv[0]);
            return arg == "--help" || arg == "-h" ? 0 : 2;
        }
    }

    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr); // before the server spawns threads

    mock_jtagd::Server server(scenario, options);
    std::println("mock-jtagd listening on {}:{}", options.bind_address, server.port());
    std::fflush(stdout);
    if (!port_file.empty())
        std::ofstream(port_file) << server.port() << '\n';

    int sig = 0;
    sigwait(&sigs, &sig);
    server.stop();
    return 0;
}
