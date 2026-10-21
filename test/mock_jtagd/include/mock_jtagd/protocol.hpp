// Wire-level definitions for the jtagd <-> AJI client protocol.
// See docs/jtagd-protocol.md for the reconstruction this is based on.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mock_jtagd {

inline constexpr std::uint16_t default_port = 1309;
inline constexpr std::string_view signature = "JTAG Server\r\n";
inline constexpr std::uint32_t current_version = 13;
inline constexpr std::size_t max_payload = 4096;

// Wire mux numbers. FIFO index i travels on mux fifo_mux_base + i.
inline constexpr unsigned mux_command = 0;
inline constexpr unsigned mux_notify = 1;
inline constexpr unsigned fifo_mux_base = 4;
inline constexpr unsigned mux_count = 16;

enum class Cmd : std::uint8_t {
    get_hardware = 0x80,
    add_hardware = 0x81,
    get_version_info = 0x83,
    get_defined_devices = 0x84,
    get_quartus_devices = 0x89,
    progress = 0x9E,
    output = 0x9F,
    remove_hardware = 0xA1,
    lock_chain = 0xA2,
    unlock_chain = 0xA3,
    scan_chain = 0xA4,
    read_chain = 0xA5,
    define_device = 0xA7,
    open_device = 0xA8,
    open_entire_chain = 0xA9,
    set_parameter = 0xAA,
    get_parameter = 0xAB,
    get_parameter_block = 0xAD,
    watch_data = 0xBE,
    close_device = 0xC0,
    lock_device = 0xC1,
    unlock_device = 0xC2,
    access_ir = 0xC5,
    access_dr = 0xC7,
    any_sequence = 0xC9,
    run_test_idle = 0xCA,
    test_logic_reset = 0xCB,
    delay_microseconds = 0xCE,
    access_ir_fifo = 0xCF,
    unlock_lock_device = 0xD3,
    authenticate_md5 = 0xF0,
    check_password = 0xF1,
    ping = 0xFD,
    use_protocol_version = 0xFE,
    continue_commands = 0xFF,
};

std::string_view to_string(Cmd cmd);

// Mirrors AJI_ERROR in aji.h; only values that fit the one-byte wire field.
enum class Err : std::uint8_t {
    no_error = 0,
    failure = 1,
    timeout = 2,
    unknown_hardware = 32,
    invalid_chain_id = 33,
    locked = 34,
    not_locked = 35,
    chain_in_use = 36,
    no_devices = 37,
    chain_not_configured = 38,
    bad_tap_position = 39,
    device_doesnt_match = 40,
    ir_length_error = 41,
    device_not_configured = 42,
    chains_claimed = 43,
    invalid_open_id = 44,
    invalid_parameter = 45,
    bad_tap_state = 46,
    too_many_devices = 47,
    ir_multiple = 48,
    bad_sequence = 49,
    instruction_claimed = 50,
    file_error = 80,
    net_down = 81,
    server_error = 82,
    no_memory = 83,
    bad_port = 84,
    port_in_use = 85,
    bad_hardware = 86,
    bad_jtag_chain = 87,
    server_active = 88,
    not_permitted = 89,
    hardware_disabled = 90,
    unimplemented = 126,
    internal_error = 127,
};

// AJI_CLAIM_TYPE values (aji.h).
namespace claim {
inline constexpr std::uint32_t ir = 0x0000;
inline constexpr std::uint32_t ir_shared = 0x0100;
inline constexpr std::uint32_t ir_shared_overlay = 0x0300;
inline constexpr std::uint32_t ir_overlaid = 0x0400;
inline constexpr std::uint32_t ir_shared_overlaid = 0x0500;
inline constexpr std::uint32_t ir_weak = 0x0800;
inline constexpr std::uint32_t overlay = 0x0001;
inline constexpr std::uint32_t overlay_shared = 0x0101;
inline constexpr std::uint32_t overlay_weak = 0x0801;
inline constexpr std::uint32_t shared_bit = 0x0100;
inline constexpr std::uint32_t weak_bit = 0x0800;
inline constexpr std::uint32_t overlay_bit = 0x0001;
} // namespace claim

struct ProtocolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Big-endian block encoder. Blocks are [code][0][len16] + body, len includes header.
class Writer {
public:
    void begin_block(std::uint8_t code);
    void end_block();
    void end_block(std::uint8_t code); // also overwrite the block's code byte

    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void str(std::string_view s);
    void raw(std::span<const std::uint8_t> bytes);

    std::span<const std::uint8_t> bytes() const { return buf_; }
    std::size_t size() const { return buf_.size(); }
    bool empty() const { return buf_.empty(); }
    void clear() { buf_.clear(); block_start_ = npos; }

private:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    std::vector<std::uint8_t> buf_;
    std::size_t block_start_ = npos;
};

// Big-endian decoder over one block body (or an un-framed FIFO payload).
// Throws ProtocolError on underrun.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

    std::uint32_t u32();
    std::uint64_t u64();
    std::string str();
    std::vector<std::uint8_t> raw(std::size_t n);

    std::size_t remaining() const { return data_.size() - pos_; }
    bool at_end() const { return remaining() == 0; }

private:
    void need(std::size_t n) const;
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
};

struct Block {
    std::uint8_t code;
    std::span<const std::uint8_t> body;
};

// Split a mux-0 payload into blocks. Throws ProtocolError on a malformed length.
std::vector<Block> split_blocks(std::span<const std::uint8_t> payload);

} // namespace mock_jtagd
