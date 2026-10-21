// mock-jtagd: run the mock JTAG server as a standalone process.
#include "mock_jtagd/scenario_yaml.hpp"
#include "mock_jtagd/server.hpp"

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <print>
#include <string>
#include <string_view>

namespace {

void usage(const char * argv0)
{
    std::println(stderr,
                 "Usage: {} [--scenario FILE.yaml [--check]] [--port N] [--bind ADDR]\n"
                 "          [--protocol-version V] [--password PW] [--allow-remote]\n"
                 "          [--port-file FILE] [--trace]\n"
                 "Serves the scenario (default: a built-in demo board) until SIGINT/SIGTERM.\n"
                 "--check validates the scenario, prints it and exits. --port 0 (default)\n"
                 "picks a free port; --port-file writes it out for test harnesses.",
                 argv0);
}

} // namespace

int main(int argc, char ** argv)
{
    mock_jtagd::Scenario scenario = mock_jtagd::Scenario::default_board();
    mock_jtagd::ServerOptions options;
    std::string port_file;
    std::string scenario_file;
    bool check = false;
    std::optional<std::uint32_t> version;
    std::optional<std::string> password;
    bool allow_remote = false;

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
        else if (arg == "--scenario")
            scenario_file = value();
        else if (arg == "--check")
            check = true;
        else if (arg == "--protocol-version")
            version = static_cast<std::uint32_t>(std::stoul(value()));
        else if (arg == "--password")
            password = value();
        else if (arg == "--allow-remote")
            allow_remote = true;
        else if (arg == "--port-file")
            port_file = value();
        else if (arg == "--trace")
            options.trace = true;
        else {
            usage(argv[0]);
            return arg == "--help" || arg == "-h" ? 0 : 2;
        }
    }

    if (!scenario_file.empty()) {
        try {
            scenario = mock_jtagd::load_scenario(scenario_file);
        } catch (const mock_jtagd::ScenarioError & e) {
            std::println(stderr, "{}", e.what());
            return 1;
        }
    }
    // Command-line settings override the scenario file.
    if (version)
        scenario.version = *version;
    if (password)
        scenario.password = password;
    if (allow_remote)
        scenario.server_flags |= 1;
    if (check) {
        std::print("{}", mock_jtagd::describe(scenario));
        return 0;
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
