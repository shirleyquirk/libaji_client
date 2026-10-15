// Example: the shortest useful program. Read the IDCODE of one TAP.
//
//   read_idcode <cable-substring> [tap-position=0] [idcode-instruction=6]
//
// Shows: find, Chain::locked, ChainLock::open_tap, ChainLock::with, a scan
// returning Pending<Bits>, and how errors come out.
#include "aji/aji.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace std::chrono_literals;

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

    // Lock the chain for the duration of the callback.
    auto idcode = chain->locked(10s, [&](auto cl) -> aji::expected<aji::Bits> {
        // Claim the IDCODE instruction (shared: other tools may use it too).
        auto tap = cl.open_tap(position, aji::ClaimSet{}.add(aji::ClaimType::ir_shared, idcode_ir), "read_idcode");
        if (!tap)
            return aji::unexpected(tap.error());

        // Hand the chain's lock to the TAP, scan, hand it back.
        return cl.with(*tap, aji::Pack::never, [&](auto s) {
            (void)s.ir(idcode_ir);
            return s.dr({.length = 32, .read_length = 32});
        });
    });   // the TAP is closed and the chain unlocked here

    if (!idcode) {
        std::fprintf(stderr, "%s\n", idcode.error().message().c_str());   // e.g. "aji_lock_chain: chain_in_use"
        return 1;
    }
    const auto& b = idcode->bytes;
    std::printf("IDCODE %02X%02X%02X%02X\n", b[3], b[2], b[1], b[0]);   // LSB first on the wire
    return 0;
}
