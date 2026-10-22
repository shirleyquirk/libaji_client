// jtag-proxy: capture what an AJI client does to a real jtagd, and turn the
// capture into mock-jtagd scenario YAML.
#include "jtag_proxy/proxy.hpp"
#include "jtag_proxy/transcript.hpp"
#include "mock_jtagd/scenario_yaml.hpp"

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {

void usage()
{
    std::println(stderr, R"(Usage:
  jtag-proxy capture [--upstream HOST:PORT] [--listen PORT] [--bind ADDR]
                     [--scan-log FILE.yaml] [--wire-log FILE.txt]
                     [--names SCENARIO.yaml] [--port-file FILE] [--quiet]
      Listen for AJI clients and forward them to the real jtagd (default
      127.0.0.1:1309), logging every logical scan. Point the client at the
      proxy with a QUARTUS_JTAG_CLIENT_CONFIG file containing
          ReplaceLocalJtagServer = "127.0.0.1:<listen port>";
      Runs until SIGINT/SIGTERM.

  jtag-proxy transcript --scan-log FILE.yaml (--node IDCODE | --tap N)
                        --command-ir IR --response-ir IR
                        [--word-bits 32] [--strip-idle WORD] [--name NAME]
      Print a scenario `channels:` snippet (provenance: observed) built from
      the captured command/response scans of one node or TAP.)");
}

std::uint64_t num(const std::string & s) { return std::stoull(s, nullptr, 0); }

int capture(const std::vector<std::string> & args)
{
    jtag_proxy::ProxyOptions o;
    o.echo = true;
    std::string port_file;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto value = [&] {
            if (i + 1 >= args.size()) {
                usage();
                std::exit(2);
            }
            return args[++i];
        };
        const std::string & a = args[i];
        if (a == "--upstream") {
            const std::string v = value();
            const auto colon = v.rfind(':');
            o.upstream_host = v.substr(0, colon);
            if (colon != std::string::npos)
                o.upstream_port = static_cast<std::uint16_t>(num(v.substr(colon + 1)));
        } else if (a == "--listen")
            o.listen_port = static_cast<std::uint16_t>(num(value()));
        else if (a == "--bind")
            o.bind_address = value();
        else if (a == "--scan-log")
            o.scan_log = value();
        else if (a == "--wire-log")
            o.wire_log = value();
        else if (a == "--names")
            o.names = mock_jtagd::load_scenario(value());
        else if (a == "--port-file")
            port_file = value();
        else if (a == "--quiet")
            o.echo = false;
        else {
            usage();
            return 2;
        }
    }

    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

    jtag_proxy::Proxy proxy(o);
    std::println("jtag-proxy listening on {}:{} -> {}:{}", o.bind_address, proxy.port(), o.upstream_host,
                 o.upstream_port);
    std::println("client config: ReplaceLocalJtagServer = \"127.0.0.1:{}\";", proxy.port());
    std::fflush(stdout);
    if (!port_file.empty())
        std::ofstream(port_file) << proxy.port() << '\n';
    int sig = 0;
    sigwait(&sigs, &sig);
    proxy.stop();
    std::println(stderr, "jtag-proxy: {} scans captured, {} decode errors", proxy.scans().size(),
                 proxy.decode_errors().size());
    return proxy.decode_errors().empty() ? 0 : 1;
}

int transcript(const std::vector<std::string> & args)
{
    std::string log, name = "captured";
    jtag_proxy::ChannelSelector sel;
    bool have_cmd = false, have_resp = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto value = [&] {
            if (i + 1 >= args.size()) {
                usage();
                std::exit(2);
            }
            return args[++i];
        };
        const std::string & a = args[i];
        if (a == "--scan-log")
            log = value();
        else if (a == "--node")
            sel.node_idcode = static_cast<std::uint32_t>(num(value()));
        else if (a == "--tap")
            sel.tap = static_cast<std::uint32_t>(num(value()));
        else if (a == "--command-ir") {
            sel.command_ir = static_cast<std::uint32_t>(num(value()));
            have_cmd = true;
        } else if (a == "--response-ir") {
            sel.response_ir = static_cast<std::uint32_t>(num(value()));
            have_resp = true;
        } else if (a == "--word-bits")
            sel.word_bits = static_cast<std::uint32_t>(num(value()));
        else if (a == "--strip-idle")
            sel.strip_leading_idle = num(value());
        else if (a == "--name")
            name = value();
        else {
            usage();
            return 2;
        }
    }
    if (log.empty() || !have_cmd || !have_resp || sel.node_idcode.has_value() == sel.tap.has_value()) {
        usage();
        return 2;
    }
    const auto result = jtag_proxy::extract_transcript(jtag_proxy::load_scan_log(log), sel);
    std::print("{}", jtag_proxy::transcript_yaml(result, sel, name, log));
    for (const auto & w : result.warnings)
        std::println(stderr, "warning: {}", w);
    return 0;
}

} // namespace

int main(int argc, char ** argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string_view cmd = argv[1];
    const std::vector<std::string> args(argv + 2, argv + argc);
    try {
        if (cmd == "capture")
            return capture(args);
        if (cmd == "transcript")
            return transcript(args);
    } catch (const std::exception & e) {
        std::println(stderr, "jtag-proxy: {}", e.what());
        return 1;
    }
    usage();
    return cmd == "--help" || cmd == "-h" ? 0 : 2;
}
