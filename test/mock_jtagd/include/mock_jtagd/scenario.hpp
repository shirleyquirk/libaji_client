// What the mock server pretends to have attached: cables, TAP chains and
// (optionally) an SLD hub with virtual-JTAG nodes on a TAP.
//
// Usually loaded from YAML (scenario_yaml.hpp); see docs/mock-jtagd.md for the
// file format and docs/mock-jtagd-design.md for why it looks like this.
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
inline constexpr std::uint32_t devfeat_bad_ir_capture = 0x1;
inline constexpr std::uint32_t devfeat_user01_break_ir = 0x2;
inline constexpr std::uint32_t devfeat_possible_hub = 0x4;
inline constexpr std::uint32_t devfeat_command_response_chain = 0x8;
inline constexpr std::uint32_t feature_jtag = 0x0800;
inline constexpr std::uint32_t chain_type_jtag = 1;

// Node IDCODE layout: [31:27] version, [26:19] node id, [18:8] mfg, [7:0] instance.
constexpr std::uint32_t sld_node_idcode(std::uint32_t version, std::uint32_t node_id,
                                        std::uint32_t mfg, std::uint32_t instance)
{
    return (version << 27) | ((node_id & 0xFF) << 19) | ((mfg & 0x7FF) << 8) | (instance & 0xFF);
}

// How a data register behaves when scanned.
struct RegisterSpec {
    enum class Kind {
        rw,       // capture returns what the previous scan wrote (initially `value`)
        constant, // capture always returns `value`
        sequence, // capture returns values[0], values[1], ... and then repeats the last
    };
    Kind kind = Kind::rw;
    std::uint32_t length = 0;            // 0: rw register takes whatever length is scanned
    std::uint64_t value = 0;
    std::vector<std::uint64_t> values;
    std::string provenance;              // observed / documented / inferred / placeholder
};

// One recorded request/response pair on a channel, in words.
struct Exchange {
    std::vector<std::uint64_t> request;
    std::vector<std::uint64_t> response;
};

// A command/response pipe on a node, as used by packet services: DR scans with
// the command IR selected append words to a request; once the request equals
// a transcript entry, that entry's response is queued and DR scans with the
// response IR selected read it out. A request that cannot match any entry is
// a model error, which fails the test (see Server::take_model_errors).
struct Channel {
    std::string name;
    std::uint32_t command_ir = 0;
    std::uint32_t response_ir = 0;
    std::uint32_t word_bits = 32;
    std::uint64_t idle = 0;              // read when no response is pending
    std::vector<Exchange> transcript;
    std::string provenance;
};

struct SldNode {
    SldNode() = default;
    SldNode(std::uint32_t idcode_, std::string name_ = {}) : name(std::move(name_)), idcode(idcode_) {}

    std::string name;
    std::uint32_t idcode = 0;
    std::map<std::uint32_t, RegisterSpec> registers; // keyed by node IR value; others are rw
    std::vector<Channel> channels;
    std::string provenance;
};

struct SldHub {
    std::uint32_t hub_version = 1;   // client only accepts version 1
    std::uint32_t mfg = mfg_altera;  // client only accepts Altera
    std::uint32_t ir_bits = 8;       // width of node IR field in the USER1 register
    std::vector<SldNode> nodes;      // node select = index + 1

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
    std::map<std::uint32_t, RegisterSpec> registers; // keyed by IR value; others are rw
    std::vector<Channel> channels;           // command/response pairs on the TAP's own IRs
    std::optional<SldHub> hub;               // answers USER1/USER0 when present
    std::string provenance;
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
