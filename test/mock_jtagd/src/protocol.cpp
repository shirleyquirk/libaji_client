#include "mock_jtagd/protocol.hpp"

#include <format>

namespace mock_jtagd {

std::string_view to_string(Cmd cmd)
{
    switch (cmd) {
    case Cmd::get_hardware: return "GET_HARDWARE";
    case Cmd::add_hardware: return "ADD_HARDWARE";
    case Cmd::get_version_info: return "GET_VERSION_INFO";
    case Cmd::get_defined_devices: return "GET_DEFINED_DEVICES";
    case Cmd::get_quartus_devices: return "GET_QUARTUS_DEVICES";
    case Cmd::progress: return "PROGRESS";
    case Cmd::output: return "OUTPUT";
    case Cmd::remove_hardware: return "REMOVE_HARDWARE";
    case Cmd::lock_chain: return "LOCK_CHAIN";
    case Cmd::unlock_chain: return "UNLOCK_CHAIN";
    case Cmd::scan_chain: return "SCAN_CHAIN";
    case Cmd::read_chain: return "READ_CHAIN";
    case Cmd::define_device: return "DEFINE_DEVICE";
    case Cmd::open_device: return "OPEN_DEVICE";
    case Cmd::open_entire_chain: return "OPEN_ENTIRE_CHAIN";
    case Cmd::set_parameter: return "SET_PARAMETER";
    case Cmd::get_parameter: return "GET_PARAMETER";
    case Cmd::get_parameter_block: return "GET_PARAMETER_BLOCK";
    case Cmd::watch_data: return "WATCH_DATA";
    case Cmd::close_device: return "CLOSE_DEVICE";
    case Cmd::lock_device: return "LOCK_DEVICE";
    case Cmd::unlock_device: return "UNLOCK_DEVICE";
    case Cmd::access_ir: return "ACCESS_IR";
    case Cmd::access_dr: return "ACCESS_DR";
    case Cmd::any_sequence: return "ANY_SEQUENCE";
    case Cmd::run_test_idle: return "RUN_TEST_IDLE";
    case Cmd::test_logic_reset: return "TEST_LOGIC_RESET";
    case Cmd::delay_microseconds: return "DELAY_MICROSECONDS";
    case Cmd::access_ir_fifo: return "ACCESS_IR_FIFO";
    case Cmd::unlock_lock_device: return "UNLOCK_LOCK_DEVICE";
    case Cmd::authenticate_md5: return "AUTHENTICATE_MD5";
    case Cmd::check_password: return "CHECK_PASSWORD";
    case Cmd::ping: return "PING";
    case Cmd::use_protocol_version: return "USE_PROTOCOL_VERSION";
    case Cmd::continue_commands: return "CONTINUE_COMMANDS";
    }
    return "UNKNOWN";
}

void Writer::begin_block(std::uint8_t code)
{
    if (block_start_ != npos)
        throw std::logic_error("Writer: nested block");
    block_start_ = buf_.size();
    buf_.insert(buf_.end(), {code, 0, 0, 0});
}

void Writer::end_block()
{
    if (block_start_ == npos)
        throw std::logic_error("Writer: no open block");
    const std::size_t len = buf_.size() - block_start_;
    if (len > 0xFFFF)
        throw std::length_error("Writer: block exceeds 64 KiB");
    buf_[block_start_ + 2] = static_cast<std::uint8_t>(len >> 8);
    buf_[block_start_ + 3] = static_cast<std::uint8_t>(len);
    block_start_ = npos;
}

void Writer::end_block(std::uint8_t code)
{
    if (block_start_ == npos)
        throw std::logic_error("Writer: no open block");
    buf_[block_start_] = code;
    end_block();
}

void Writer::u32(std::uint32_t v)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        buf_.push_back(static_cast<std::uint8_t>(v >> shift));
}

void Writer::u64(std::uint64_t v)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        buf_.push_back(static_cast<std::uint8_t>(v >> shift));
}

void Writer::str(std::string_view s)
{
    if (s.size() > 255)
        throw std::length_error("Writer: string longer than 255 bytes");
    buf_.push_back(static_cast<std::uint8_t>(s.size()));
    buf_.insert(buf_.end(), s.begin(), s.end());
}

void Writer::raw(std::span<const std::uint8_t> bytes)
{
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
}

void Reader::need(std::size_t n) const
{
    if (remaining() < n)
        throw ProtocolError(std::format("block underrun: need {} bytes, have {}", n, remaining()));
}

std::uint32_t Reader::u32()
{
    need(4);
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v = (v << 8) | data_[pos_++];
    return v;
}

std::uint64_t Reader::u64()
{
    need(8);
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | data_[pos_++];
    return v;
}

std::string Reader::str()
{
    need(1);
    const std::size_t n = data_[pos_++];
    need(n);
    std::string s(reinterpret_cast<const char *>(data_.data() + pos_), n);
    pos_ += n;
    return s;
}

std::vector<std::uint8_t> Reader::raw(std::size_t n)
{
    need(n);
    std::vector<std::uint8_t> v(data_.begin() + static_cast<std::ptrdiff_t>(pos_),
                                data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
    pos_ += n;
    return v;
}

std::vector<Block> split_blocks(std::span<const std::uint8_t> payload)
{
    std::vector<Block> blocks;
    std::size_t pos = 0;
    while (pos < payload.size()) {
        if (payload.size() - pos < 4)
            throw ProtocolError("truncated block header");
        const std::size_t len = (std::size_t{payload[pos + 2]} << 8) | payload[pos + 3];
        if (len < 4 || pos + len > payload.size())
            throw ProtocolError(std::format("bad block length {} at offset {}", len, pos));
        blocks.push_back({payload[pos], payload.subspan(pos + 4, len - 4)});
        pos += len;
    }
    return blocks;
}

} // namespace mock_jtagd
