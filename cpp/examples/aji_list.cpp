// aji_list: list chains, TAPs and SLD nodes, like `jtagconfig`, using the
// C++ wrapper. Run both and compare the output.
//
//   aji_list            all chains
//   aji_list <text>     only chains whose display name contains <text>
//
// For each TAP with an SLD hub it also reads the top-level hub's design hash,
// the same way jtagconfig's print_design_hash does (jtagconfig.cpp:827-885),
// but with the deferred reads checked (FINDINGS.md §3.4) and the lock
// handed to the hub and back atomically, as the library's own code does.
#include "aji/aji.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <utility>

using namespace std::chrono_literals;

namespace {

// From jtagconfig.cpp:92-97.
constexpr std::uint64_t JTAG_USERCODE    = 0x7;
constexpr std::uint64_t JTAG_USR0        = 0xC;
constexpr std::uint64_t JTAG_USR1        = 0xE;
constexpr std::uint32_t HUB_MINORVERSION = 0x4;
constexpr std::uint32_t HUB_IDENT        = 0x5;

// Same claims as jtagconfig's IDENT_CLAIMS (jtagconfig.cpp:100-107).
aji::ClaimSet ident_claims()
{
    return aji::ClaimSet{}
        .add(aji::ClaimType::ir_shared, JTAG_USERCODE)
        .add(aji::ClaimType::ir_shared_overlay, JTAG_USR1)
        .add(aji::ClaimType::ir_shared_overlaid, JTAG_USR0)
        .add(aji::ClaimType::overlay_shared, HUB_MINORVERSION)
        .add(aji::ClaimType::overlay_shared, HUB_IDENT);
}

// The design hash of the hub that `node` hangs off at `level`, or nullopt if
// the hub is too old to have one. Borrows the chain lock: hands it to the
// hub and back into `cl` (empty afterwards only if the lock was lost).
aji::expected<std::optional<std::string>> design_hash(aji::ChainLock& cl, std::uint32_t tap,
                                                      const aji::SldNodeInfo& node, std::uint32_t level)
{
    auto hub = cl.open_hub(tap, node.hub(level), ident_claims(), "aji_list");
    if (!hub)
        return aji::unexpected(hub.error());

    auto s = cl.hand_to(*hub, aji::Pack::manual);
    if (!s)
        return aji::unexpected(s.error());
    (void)s->overlay(HUB_MINORVERSION);
    auto minor = s->dr({.length = 4, .flags = aji::DrFlag::unused_x, .read_length = 4});
    (void)s->overlay(HUB_IDENT);
    auto ident = s->dr({.length = 4, .flags = aji::DrFlag::unused_x, .read_length = 4, .batch = 32});
    auto flushed = s->flush();   // everything above was queued (Pack::manual); run it

    auto back = s->hand_back();
    if (!back)
        return aji::unexpected(back.error());
    cl = std::move(*back);
    if (!flushed)
        return aji::unexpected(flushed.error());

    auto m = minor.get();
    auto id = ident.get();
    if (!m)
        return aji::unexpected(m.error());
    if (!id)
        return aji::unexpected(id.error());
    if ((m->bytes[0] & 1) == 0)
        return std::nullopt;   // old hub: no ident register

    // Items 12..31 are the design hash, one nibble each (jtagconfig.cpp:871-876).
    std::string hash;
    for (std::uint32_t i = 12; i < 32; ++i) {
        char nibble[2];
        std::snprintf(nibble, sizeof nibble, "%X", id->item(i)[0] & 0xF);
        hash += nibble;
    }
    return hash;
}

aji::expected<void> list_taps(aji::ChainLock& cl)
{
    auto taps = cl.taps();
    if (!taps)
        return aji::unexpected(taps.error());
    if (taps->empty())
        std::printf("  No devices on chain\n");

    for (const aji::TapInfo& tap : *taps) {
        std::printf("  %08X   %s%s", tap.idcode, tap.name.value_or("(unknown)").c_str(),
                    tap.ir_length_known() ? "" : "  (IR length unknown)");
        if (tap.ir_length_known())
            std::printf("  (IR=%u)", unsigned(tap.ir_length));
        std::printf("\n");

        auto nodes = cl.nodes(tap.position);
        if (!nodes) {
            if (nodes.error().code != aji::Errc::too_many_hierarchies)
                std::printf("    (nodes: %s)\n", nodes.error().message().c_str());
            continue;
        }
        if (nodes->empty())
            continue;

        auto hash = design_hash(cl, tap.position, nodes->front(), 0);
        if (!hash)
            std::printf("    Design hash not accessible (%s)\n", hash.error().message().c_str());
        else if (*hash)
            std::printf("    Design hash    %20s\n", (*hash)->c_str());
        if (!cl)
            return aji::unexpected(aji::Error{aji::Errc::lock_lost, "aji_list", {}});

        for (const aji::SldNodeInfo& node : *nodes)
            std::printf("    %*s+ Node %08X\n", int(3 * node.depth()), "", node.idcode());
    }
    return {};
}

void list_chain(int index, const aji::Chain& chain)
{
    std::printf("%d) %s\n", index, chain.info().display_name().c_str());
    auto cl = chain.lock(10s);
    auto done = cl ? list_taps(*cl) : aji::expected<void>(aji::unexpected(cl.error()));
    if (!done)
        std::printf("  Error: %s\n", done.error().message().c_str());
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv)
{
    const std::string filter = argc > 1 ? argv[1] : "";

    auto chains = aji::scan(5s);
    if (!chains) {
        std::fprintf(stderr, "scan: %s\n", chains.error().message().c_str());
        return 1;
    }

    int index = 0;
    for (const aji::Chain& chain : *chains) {
        ++index;
        if (chain.info().is_dummy()) {
            std::printf("%d) %s\n\n", index, chain.info().display_name().c_str());
            continue;
        }
        if (!filter.empty() && chain.info().display_name().find(filter) == std::string::npos)
            continue;
        list_chain(index, chain);
    }
    return 0;
}
