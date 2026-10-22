// jtag-proxy: a fake jtagd that forwards to a real one and records what passes.
//
// Bytes are forwarded unmodified; decoding runs on a copy, so a decoder bug
// can't change what the real server sees. Two logs come out:
//   - scan log: one ScanRecord per logical IR/DR scan, with batching undone;
//   - wire log: every packet and command, for protocol-level questions.
#pragma once

#include "jtag_proxy/scan.hpp"
#include "mock_jtagd/scenario.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jtag_proxy {

struct ProxyOptions {
    std::string upstream_host = "127.0.0.1";
    std::uint16_t upstream_port = mock_jtagd::default_port;
    std::string bind_address = "127.0.0.1";
    std::uint16_t listen_port = 0;                    // 0 = ephemeral
    std::optional<std::filesystem::path> scan_log;    // YAML list, appended as scans complete
    std::optional<std::filesystem::path> wire_log;    // text
    std::optional<mock_jtagd::Scenario> names;        // to label devices/nodes by IDCODE
    bool echo = false;                                // print scans to stderr as they happen
};

class Proxy {
public:
    explicit Proxy(ProxyOptions options);
    ~Proxy();
    Proxy(const Proxy &) = delete;
    Proxy & operator=(const Proxy &) = delete;

    std::uint16_t port() const;
    void stop();

    std::vector<ScanRecord> scans() const;
    std::vector<std::string> wire_log() const;
    // Problems decoding the stream. Forwarding carries on regardless.
    std::vector<std::string> decode_errors() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace jtag_proxy
