// What the mock server pretends to have attached: cables, TAP chains and
// (optionally) an SLD hub with virtual-JTAG nodes on a TAP.
#pragma once

#include "mock_jtagd/protocol.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mock_jtagd {

// Altera/Intel conventions the client hard-codes.
inline constexpr std::uint32_t user0_instruction = 0x0C;
inline constexpr std::uint32_t user1_instruction = 0x0E;
inline constexpr std::uint32_t mfg_altera = 0x6E;

// aji.h AJI_DEVFEAT / AJI_FEATURE / AJI_CHAIN_TYPE values.
inline constexpr std::uint32_t devfeat_possible_hub = 0x4;
inline constexpr std::uint32_t feature_jtag = 0x0800;
inline constexpr std::uint32_t chain_type_jtag = 1;

// Node IDCODE layout: [31:27] version, [26:19] node id, [18:8] mfg, [7:0] instance.
constexpr std::uint32_t sld_node_idcode(std::uint32_t version, std::uint32_t node_id,
                                        std::uint32_t mfg, std::uint32_t instance)
{
    return (version << 27) | ((node_id & 0xFF) << 19) | ((mfg & 0x7FF) << 8) | (instance & 0xFF);
}

struct SldHub {
    std::uint32_t hub_version = 1;   // client only accepts version 1
    std::uint32_t mfg = mfg_altera;  // client only accepts Altera
    std::uint32_t ir_bits = 8;       // width of node IR field in the USER1 register
    std::vector<std::uint32_t> nodes; // node IDCODEs, node select = index + 1

    // Hub IDCODE: [31:27] version, [26:19] node count, [18:8] mfg, [7:0] ir bits.
    std::uint32_t idcode() const;
    // Width of the node-select field in the USER1 register: ceil(log2(n + 1)).
    std::uint32_t select_bits() const;
};

struct Tap {
    std::uint32_t idcode = 0;
    std::uint32_t ir_length = 10;
    std::string name;
    std::uint32_t features = 0;              // AJI_DEVFEAT bits, reported by READ_CHAIN
    std::uint32_t idcode_instruction = 0x6;  // TLR selects this instruction
    std::uint32_t ir_capture = 0x1;          // value returned by IR capture ('b..01)
    std::optional<SldHub> hub;               // answers USER1/USER0 when present
};

struct Cable {
    std::string hw_name = "USB-Blaster";
    std::string port;                  // e.g. "USB-0"; empty is sent as an empty string
    std::string device_name;
    std::uint32_t chain_type = chain_type_jtag;
    std::uint32_t features = feature_jtag;
    std::vector<Tap> taps;             // index 0 is nearest TDO
    std::map<std::string, std::uint32_t> parameters = {{"JtagClock", 6000000}};
};

struct Scenario {
    std::uint32_t version = current_version;     // advertised protocol version (1..13)
    std::optional<std::string> password;         // if set, client must pass MD5 auth
    std::uint32_t server_flags = 0;              // bit0 SERVER_ALLOW_REMOTE
    std::string version_info = "Mock jtagd (libaji_client test server)";
    std::string server_path = "/usr/bin/mock-jtagd";
    std::vector<Cable> cables;

    // One USB-Blaster with an HPS DAP + an FPGA carrying a two-node SLD hub.
    static Scenario default_board();
};

} // namespace mock_jtagd
