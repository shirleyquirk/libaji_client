#include "jtag_proxy/scan.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace jtag_proxy {
namespace {

std::uint64_t number(const YAML::Node & n)
{
    return std::stoull(n.Scalar(), nullptr, 0);
}

std::string yaml_quote(std::string_view s)
{
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + '"';
}

std::string window_yaml(const BitWindow & w)
{
    return std::format("{{offset: {}, bits: {}, hex: \"{}\"}}", w.offset, w.bits, w.hex());
}

BitWindow window_from(const YAML::Node & n)
{
    return BitWindow::from_hex(static_cast<std::uint32_t>(number(n["offset"])),
                               static_cast<std::uint32_t>(number(n["bits"])), n["hex"].Scalar());
}

bool bit(const std::vector<std::uint8_t> & bytes, std::size_t i)
{
    return i / 8 < bytes.size() && ((bytes[i / 8] >> (i % 8)) & 1);
}

} // namespace

std::string BitWindow::hex() const
{
    if (bits == 0)
        return "";
    std::vector<std::string> groups;
    for (std::uint32_t base = 0; base < bits; base += 32) {
        const std::uint32_t n = std::min<std::uint32_t>(32, bits - base);
        std::uint32_t v = 0;
        for (std::uint32_t i = 0; i < n; ++i)
            if (bit(bytes, base + i))
                v |= 1u << i;
        groups.push_back(std::format("{:0{}X}", v, (n + 3) / 4));
    }
    std::string out;
    for (auto it = groups.rbegin(); it != groups.rend(); ++it)
        out += (out.empty() ? "" : "_") + *it;
    return out;
}

std::vector<std::uint64_t> BitWindow::words(std::uint32_t word_bits) const
{
    std::vector<std::uint64_t> out;
    for (std::uint32_t base = 0; base < bits; base += word_bits) {
        std::uint64_t v = 0;
        for (std::uint32_t i = 0; i < word_bits && base + i < bits; ++i)
            if (bit(bytes, base + i))
                v |= std::uint64_t{1} << i;
        out.push_back(v);
    }
    return out;
}

BitWindow BitWindow::from_hex(std::uint32_t offset, std::uint32_t bits, std::string_view hex)
{
    BitWindow w{offset, bits, std::vector<std::uint8_t>((bits + 7) / 8, 0)};
    // Groups are most-significant first, each covering 32 bits (the top one
    // possibly fewer): walk them from the end.
    std::vector<std::string> groups;
    std::istringstream in{std::string(hex)};
    for (std::string g; std::getline(in, g, '_');)
        groups.push_back(g);
    std::uint32_t base = 0;
    for (auto it = groups.rbegin(); it != groups.rend(); ++it, base += 32) {
        const std::uint64_t v = it->empty() ? 0 : std::stoull(*it, nullptr, 16);
        for (std::uint32_t i = 0; i < 32 && base + i < bits; ++i)
            if ((v >> i) & 1)
                w.bytes[(base + i) / 8] |= static_cast<std::uint8_t>(1u << ((base + i) % 8));
    }
    return w;
}

std::string ScanRecord::to_text() const
{
    std::string where = std::format("s{} #{} {}", session, seq, cable.empty() ? std::format("chain{}", chain_id) : cable);
    if (tap)
        where += std::format(" tap{} {}", *tap, device);
    else
        where += " whole-chain";
    if (node_select)
        where += std::format(" {} ({:08X}) IR 0x{:X}", node, node_idcode, node_ir.value_or(0));
    else if (ir && op == Op::dr)
        where += std::format(" IR 0x{:X}", *ir);

    std::string what;
    switch (op) {
    case Op::ir:
        what = write.empty() ? std::format("IR <- 0x{:X}", ir.value_or(0)) : std::format("IR {}b w={}", length, write.hex());
        if (ir_capture)
            what += std::format(" captured 0x{:X}", *ir_capture);
        if (!read.empty())
            what += std::format(" r={}", read.hex());
        break;
    case Op::dr:
        what = std::format("DR {}b", length);
        if (!write.empty())
            what += std::format(" w{}={}", write.offset ? std::format("@{}", write.offset) : "", write.hex());
        if (!read.empty())
            what += std::format(" r{}={}", read.offset ? std::format("@{}", read.offset) : "", read.hex());
        break;
    case Op::any_sequence:
        what = std::format("ANY_SEQUENCE {} tcks tdi={}{}", length, write.hex(),
                           read.empty() ? "" : std::format(" tdo={}", read.hex()));
        break;
    }
    if (!executed)
        what += "  (skipped)";
    else if (result != mock_jtagd::Err::no_error)
        what += std::format("  error {}", static_cast<int>(result));
    return std::format("{}  {}", where, what);
}

std::string ScanRecord::to_yaml() const
{
    std::string out = std::format("{{seq: {}, session: {}, t_ms: {:.3f}, op: {}, chain: {}, cable: {}", seq, session,
                                  t_ms, op == Op::ir ? "ir" : op == Op::dr ? "dr" : "any_sequence", chain_id,
                                  yaml_quote(cable));
    if (tap)
        out += std::format(", tap: {}, device: {}, device_idcode: 0x{:08X}", *tap, yaml_quote(device), device_idcode);
    if (ir)
        out += std::format(", ir: 0x{:X}", *ir);
    if (node_select)
        out += std::format(", node_select: {}, node_idcode: 0x{:08X}, node_ir: 0x{:X}, node: {}", *node_select,
                           node_idcode, node_ir.value_or(0), yaml_quote(node));
    out += std::format(", length: {}", length);
    if (!write.empty())
        out += std::format(", write: {}", window_yaml(write));
    if (!read.empty())
        out += std::format(", read: {}", window_yaml(read));
    if (ir_capture)
        out += std::format(", ir_capture: 0x{:X}", *ir_capture);
    out += std::format(", result: {}, executed: {}}}", static_cast<int>(result), executed);
    return out;
}

std::vector<ScanRecord> load_scan_log(const std::filesystem::path & file)
{
    std::ifstream in(file);
    if (!in)
        throw std::runtime_error(std::format("{}: cannot open", file.string()));
    std::stringstream ss;
    ss << in.rdbuf();
    const YAML::Node root = YAML::Load(ss.str());
    std::vector<ScanRecord> out;
    if (!root || root.IsNull())
        return out;
    for (const auto & n : root) {
        ScanRecord r;
        r.seq = number(n["seq"]);
        r.session = static_cast<unsigned>(number(n["session"]));
        r.t_ms = n["t_ms"].as<double>();
        const std::string op = n["op"].Scalar();
        r.op = op == "ir" ? ScanRecord::Op::ir : op == "dr" ? ScanRecord::Op::dr : ScanRecord::Op::any_sequence;
        r.chain_id = static_cast<std::uint32_t>(number(n["chain"]));
        r.cable = n["cable"].Scalar();
        if (n["tap"]) {
            r.tap = static_cast<std::uint32_t>(number(n["tap"]));
            r.device = n["device"].Scalar();
            r.device_idcode = static_cast<std::uint32_t>(number(n["device_idcode"]));
        }
        if (n["ir"])
            r.ir = static_cast<std::uint32_t>(number(n["ir"]));
        if (n["node_select"]) {
            r.node_select = static_cast<std::uint32_t>(number(n["node_select"]));
            r.node_idcode = static_cast<std::uint32_t>(number(n["node_idcode"]));
            r.node_ir = static_cast<std::uint32_t>(number(n["node_ir"]));
            r.node = n["node"].Scalar();
        }
        r.length = static_cast<std::uint32_t>(number(n["length"]));
        if (n["write"])
            r.write = window_from(n["write"]);
        if (n["read"])
            r.read = window_from(n["read"]);
        if (n["ir_capture"])
            r.ir_capture = static_cast<std::uint32_t>(number(n["ir_capture"]));
        r.result = static_cast<mock_jtagd::Err>(number(n["result"]));
        r.executed = n["executed"].as<bool>();
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace jtag_proxy
