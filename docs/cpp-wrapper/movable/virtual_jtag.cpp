// The core of cpp/examples/virtual_jtag.cpp, written against the movable
// sketch: discovery under the chain lock, then an atomic handoff to the node.
#include "sketch.hpp"

#include <optional>
#include <vector>

aji::expected<std::vector<aji::Bits>> transfer(aji2::ChainLock& cl, std::uint32_t tap, const aji::SldNodeInfo& found,
                                               std::uint32_t vir, aji::BitsView data, std::uint32_t count)
{
    auto node = cl.open_node(tap, found,
                             aji::ClaimSet{}
                                 .add(aji::ClaimType::ir_shared_overlay, 0xE)
                                 .add(aji::ClaimType::ir_shared_overlaid, 0xC)
                                 .add(aji::ClaimType::overlay_shared, vir),
                             "virtual_jtag");
    if (!node)
        return aji::unexpected(node.error());

    auto s = cl.hand_to(*node, aji::Pack::manual);
    if (!s)
        return aji::unexpected(s.error());

    (void)s->overlay(vir);
    std::vector<aji::Pending<aji::Bits>> pending;
    for (std::uint32_t i = 0; i < count; ++i)
        pending.push_back(s->dr({.length = data.length, .write = data, .read_length = data.length}));

    // Give the lock back to the caller's ChainLock (it was emptied by hand_to).
    auto back = s->hand_back();
    if (!back)
        return aji::unexpected(back.error());
    cl = std::move(*back);

    std::vector<aji::Bits> out;
    for (auto& p : pending) {
        auto bits = p.get();
        if (!bits)
            return aji::unexpected(bits.error());
        out.push_back(std::move(*bits));
    }
    return out;
}
