// Example: the shortest useful program. Read the IDCODE of one TAP.
//
//   read_idcode <cable-substring> [tap-position=0] [idcode-instruction=6]
//
// Shows: find, Chain::lock, ChainLock::open_tap, the atomic handoff of the
// chain's lock to the TAP (hand_to), a scan returning Pending<Bits>, and how
// errors come out.
#include "aji/aji.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace std::chrono_literals;

aji::expected<aji::Bits> read_idcode(const aji::Chain& chain, std::uint32_t position, std::uint32_t idcode_ir)
{
    auto cl = chain.lock(10s);
    if (!cl)
        return aji::unexpected(cl.error());   // e.g. "aji_lock_chain: chain_in_use"

    // Claim the IDCODE instruction (shared: other tools may use it too).
    auto tap = cl->open_tap(position, aji::ClaimSet{}.add(aji::ClaimType::ir_shared, idcode_ir), "read_idcode");
    if (!tap)
        return aji::unexpected(tap.error());

    // The chain's lock moves to the TAP in one step; *cl is empty afterwards.
    auto s = cl->hand_to(*tap, aji::Pack::never);
    if (!s)
        return aji::unexpected(s.error());
    (void)s->ir(idcode_ir);
    return s->dr({.length = 32, .read_length = 32}).get();   // Pack::never: ready at once
}   // ~s unlocks, ~tap closes, ~cl is already empty

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <cable-substring> [tap-position=0] [idcode-instruction=6]\n", argv[0]);
        return 2;
    }
    const std::string cable = argv[1];
    const std::uint32_t position = argc > 2 ? std::uint32_t(std::strtoul(argv[2], nullptr, 0)) : 0;
    const std::uint32_t idcode_ir = argc > 3 ? std::uint32_t(std::strtoul(argv[3], nullptr, 0)) : 6;

    auto chain = aji::find([&](const aji::HardwareInfo& h) {
        return !h.is_dummy() && h.display_name().find(cable) != std::string::npos;
    });
    if (!chain) {
        std::fprintf(stderr, "%s\n", chain.error().message().c_str());
        return 1;
    }

    auto idcode = read_idcode(*chain, position, idcode_ir);
    if (!idcode) {
        std::fprintf(stderr, "%s\n", idcode.error().message().c_str());
        return 1;
    }
    const auto& b = idcode->bytes;
    std::printf("IDCODE %02X%02X%02X%02X\n", b[3], b[2], b[1], b[0]);   // LSB first on the wire
    return 0;
}
