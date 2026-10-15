// Example: talk to a Virtual JTAG (sld_virtual_jtag) instance in an FPGA.
//
//   virtual_jtag <cable-substring> <instance> <vir> <dr-bits> [hex-data] [count=1]
//
// Writes <vir> to the instance's virtual IR, then shifts <dr-bits> of
// <hex-data> (default 0) through its virtual DR <count> times, printing what
// came back each time. What the DR means is up to your design.
//
// Shows: discovery under the chain lock, then the lock handed atomically to
// the node and back (no other client can reconfigure the FPGA in between),
// a helper function that borrows the ChainLock, Session<Kind::sld_node>
// (overlay + dr; `ir` doesn't compile for nodes), and Pack::manual with
// several Pendings read after one flush.
#include "aji/aji.hpp"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

// Intel USER1/USER0 instructions (jtagconfig.cpp:93-94): the virtual IR and
// virtual DR of every SLD node are reached through them.
constexpr std::uint64_t USER0 = 0xC;
constexpr std::uint64_t USER1 = 0xE;

// SLD node id fields (jtagconfig.cpp:791-794).
std::uint32_t node_instance(std::uint32_t id) { return id & 0xFF; }
std::uint32_t node_vendor(std::uint32_t id) { return (id >> 8) & 0x7FF; }
std::uint32_t node_type(std::uint32_t id) { return (id >> 19) & 0xFF; }
constexpr std::uint32_t INTEL = 110, VIRTUAL_JTAG = 8;

struct Found {
    std::uint32_t    tap_position;
    aji::SldNodeInfo node;
};

// Discovery: every TAP with an SLD hub lists its nodes.
aji::expected<Found> find_instance(aji::ChainLock& cl, std::uint32_t instance)
{
    auto taps = cl.taps();
    if (!taps)
        return aji::unexpected(taps.error());
    for (const aji::TapInfo& tap : *taps) {
        auto nodes = cl.nodes(tap.position);
        if (!nodes)
            continue;   // no hub, or not readable: not ours
        for (const aji::SldNodeInfo& n : *nodes)
            if (node_vendor(n.idcode()) == INTEL && node_type(n.idcode()) == VIRTUAL_JTAG &&
                node_instance(n.idcode()) == instance)
                return Found{tap.position, n};
    }
    return aji::unexpected(aji::Error{aji::Errc::no_matching_nodes, "virtual_jtag: no such instance", {}});
}

// Borrows the caller's chain lock: hands it to the node, scans, hands it
// back into `cl`. If the handback loses the lock, `cl` stays empty.
aji::expected<std::vector<aji::Bits>> transfer(aji::ChainLock& cl, const Found& where, std::uint32_t vir,
                                               aji::BitsView data, std::uint32_t count)
{
    auto node = cl.open_node(where.tap_position, where.node,
                             aji::ClaimSet{}
                                 .add(aji::ClaimType::ir_shared_overlay, USER1)
                                 .add(aji::ClaimType::ir_shared_overlaid, USER0)
                                 .add(aji::ClaimType::overlay_shared, vir),
                             "virtual_jtag");
    if (!node)
        return aji::unexpected(node.error());

    auto s = cl.hand_to(*node, aji::Pack::manual);   // cl is empty until hand_back
    if (!s)
        return aji::unexpected(s.error());

    (void)s->overlay(vir);
    std::vector<aji::Pending<aji::Bits>> pending;
    for (std::uint32_t i = 0; i < count; ++i)
        pending.push_back(s->dr({.length = data.length, .write = data, .read_length = data.length}));
    // Before the flush every pending.get() would be Errc::not_flushed.
    auto flushed = s->flush();

    auto back = s->hand_back();
    if (!back)
        return aji::unexpected(back.error());
    cl = std::move(*back);
    if (!flushed)
        return aji::unexpected(flushed.error());

    std::vector<aji::Bits> out;
    for (auto& p : pending) {
        auto bits = p.get();
        if (!bits)
            return aji::unexpected(bits.error());
        out.push_back(std::move(*bits));
    }
    return out;
}   // ~node: closed (not locked any more); ~s: empty

// "1A2B" -> little-endian bytes, `bits` long.
std::vector<std::uint8_t> parse_hex(const std::string& hex, std::uint32_t bits)
{
    std::vector<std::uint8_t> out((bits + 7) / 8, 0);
    std::uint32_t nibble = 0;
    for (auto it = hex.rbegin(); it != hex.rend() && nibble / 2 < out.size(); ++it, ++nibble) {
        const std::uint8_t v = std::uint8_t(std::strtoul(std::string(1, *it).c_str(), nullptr, 16));
        out[nibble / 2] |= std::uint8_t(v << (4 * (nibble % 2)));
    }
    return out;
}

std::string to_hex(const aji::Bits& b, std::uint32_t item)
{
    std::string s;
    auto bytes = b.item(item);
    for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
        char buf[3];
        std::snprintf(buf, sizeof buf, "%02X", *it);
        s += buf;
    }
    return s;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <cable-substring> <instance> <vir> <dr-bits> [hex-data] [count=1]\n", argv[0]);
        return 2;
    }
    const std::string cable = argv[1];
    const std::uint32_t instance = std::uint32_t(std::strtoul(argv[2], nullptr, 0));
    const std::uint32_t vir = std::uint32_t(std::strtoul(argv[3], nullptr, 0));
    const std::uint32_t dr_bits = std::uint32_t(std::strtoul(argv[4], nullptr, 0));
    const auto data = parse_hex(argc > 5 ? argv[5] : "0", dr_bits);
    const std::uint32_t count = argc > 6 ? std::uint32_t(std::strtoul(argv[6], nullptr, 0)) : 1;

    auto chain = aji::find([&](const aji::HardwareInfo& h) {
        return !h.is_dummy() && h.display_name().find(cable) != std::string::npos;
    });
    if (!chain) {
        std::fprintf(stderr, "%s\n", chain.error().message().c_str());
        return 1;
    }

    auto cl = chain->lock(10s);
    if (!cl) {
        std::fprintf(stderr, "%s\n", cl.error().message().c_str());
        return 1;
    }
    auto where = find_instance(*cl, instance);
    if (!where) {
        std::fprintf(stderr, "%s\n", where.error().message().c_str());
        return 1;
    }
    auto results = transfer(*cl, *where, vir, aji::BitsView{data, dr_bits}, count);
    if (!results) {
        std::fprintf(stderr, "%s\n", results.error().message().c_str());
        return 1;
    }
    for (std::size_t i = 0; i < results->size(); ++i)
        std::printf("transfer %zu: read %s\n", i, to_hex((*results)[i], 0).c_str());
    return 0;
}
