// One logical JTAG scan as seen by jtag-proxy, independent of how the client
// batched it on the wire. See docs/mock-jtagd-design.md ("Capture").
#pragma once

#include "mock_jtagd/protocol.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace jtag_proxy {

// A window of a scan: `bits` bits starting at bit `offset` of the register,
// LSB-first in `bytes` (the AJI buffer convention).
struct BitWindow {
    std::uint32_t offset = 0;
    std::uint32_t bits = 0;
    std::vector<std::uint8_t> bytes;

    bool empty() const { return bits == 0; }
    // Most-significant first, 32-bit groups separated by '_' (e.g. "4341A0DD_00001000").
    std::string hex() const;
    // Split into `word_bits`-bit words, first-shifted word first.
    std::vector<std::uint64_t> words(std::uint32_t word_bits) const;
    static BitWindow from_hex(std::uint32_t offset, std::uint32_t bits, std::string_view hex);
};

struct ScanRecord {
    enum class Op { ir, dr, any_sequence };

    std::uint64_t seq = 0;              // order within the capture
    unsigned session = 0;               // client connection, 1-based
    double t_ms = 0;                    // since proxy start
    Op op = Op::dr;

    std::uint32_t chain_id = 0;
    std::string cable;                  // "USB-BlasterII [1-3]"
    std::optional<std::uint32_t> tap;   // none: whole-chain open
    std::string device;                 // device name from READ_CHAIN
    std::uint32_t device_idcode = 0;

    std::optional<std::uint32_t> ir;    // TAP instruction in effect (DR), or written (IR)
    // Set when the scan went through the SLD hub to a node (USER0 with a node selected).
    std::optional<std::uint32_t> node_select;
    std::uint32_t node_idcode = 0;      // 0 if the hub table wasn't seen
    std::optional<std::uint32_t> node_ir;
    std::string node;                   // name from --names scenario, else "node<select>"

    std::uint32_t length = 0;
    BitWindow write;
    BitWindow read;
    std::optional<std::uint32_t> ir_capture; // ACCESS_IR captured value
    mock_jtagd::Err result = mock_jtagd::Err::no_error;
    bool executed = true;               // false: skipped by the server after an earlier error

    // "s1 #12 USB-BlasterII [1-3] tap1 5CSEMA5 node 2 sdm (0BD06E00) IR 0x1  DR 32b  w=00000010"
    std::string to_text() const;
    // One flow-style YAML mapping (a scan log is a YAML list of these).
    std::string to_yaml() const;
};

std::vector<ScanRecord> load_scan_log(const std::filesystem::path & file);

} // namespace jtag_proxy
