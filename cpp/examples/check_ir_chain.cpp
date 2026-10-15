// Example: the whole chain as one shift register (Kind::entire_chain).
//
//   check_ir_chain <cable-substring>
//
// Shifts all-ones through every IR on the chain (BYPASS everywhere) and
// checks the captured value: IEEE 1149.1 makes every TAP capture ...01 into
// its IR. Knowing each TAP's IR length, that tells us which end of the chain
// TapInfo::position 0 is, which DESIGN.md O7 asks (aji.h says nearest TDI,
// jtag_client_open.h says nearest TDO). Then checks the DR: in BYPASS, each
// TAP is one bit that captures 0.
//
// Needs exclusive access: fails with chain_in_use if another tool has any
// TAP on this chain open.
#include "aji/aji.hpp"

#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

// Does the captured IR show "...01" at each TAP's offset, if TAP i's IR starts
// at offsets[i] (bit 0 = first bit out of TDO)?
bool capture_matches(const aji::Bits& captured, const std::vector<aji::TapInfo>& taps,
                     const std::vector<std::uint32_t>& offsets)
{
    for (std::size_t i = 0; i < taps.size(); ++i) {
        if (taps[i].features.has(aji::DeviceFeature::bad_ir_capture))
            continue;   // this device doesn't promise ...01
        if (!captured.bit(offsets[i]) || captured.bit(offsets[i] + 1))
            return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <cable-substring>\n", argv[0]);
        return 2;
    }
    const std::string cable = argv[1];

    auto chain = aji::find([&](const aji::HardwareInfo& h) {
        return !h.is_dummy() && h.display_name().find(cable) != std::string::npos;
    });
    if (!chain) {
        std::fprintf(stderr, "%s\n", chain.error().message().c_str());
        return 1;
    }

    auto done = chain->locked(10s, [](auto cl) -> aji::expected<void> {
        auto taps = cl.taps();
        if (!taps)
            return aji::unexpected(taps.error());
        if (taps->empty())
            return aji::unexpected(aji::Error{aji::Errc::no_devices, "check_ir_chain", {}});
        for (const auto& t : *taps)
            if (!t.ir_length_known())
                return aji::unexpected(aji::Error{aji::Errc::ir_length_error, "check_ir_chain: unknown IR length", {}});

        const std::uint32_t ir_total = std::accumulate(taps->begin(), taps->end(), 0u,
                                                       [](std::uint32_t n, const auto& t) { return n + t.ir_length; });
        const std::uint32_t taps_n = std::uint32_t(taps->size());

        auto whole = cl.open_entire(aji::ChainType::jtag, "check_ir_chain");
        if (!whole)
            return aji::unexpected(whole.error());

        struct Captured { aji::Bits ir, dr; };
        auto captured = cl.with(*whole, aji::Pack::manual, [&](auto s) -> aji::expected<Captured> {
            const std::vector<std::uint8_t> ones((std::max(ir_total, taps_n) + 7) / 8, 0xFF);
            auto ir = s.ir(aji::BitsView{ones, ir_total});   // BYPASS into every TAP
            auto dr = s.dr({.length = taps_n, .write = aji::BitsView{ones, taps_n}, .read_length = taps_n});
            if (auto f = s.flush(); !f)
                return aji::unexpected(f.error());
            auto i = ir.get();
            auto d = dr.get();
            if (!i) return aji::unexpected(i.error());
            if (!d) return aji::unexpected(d.error());
            return Captured{*i, *d};
        });
        if (!captured)
            return aji::unexpected(captured.error());

        // Offsets of each TAP's IR in the captured stream under both orders.
        std::vector<std::uint32_t> pos0_at_tdo(taps_n), pos0_at_tdi(taps_n);
        for (std::uint32_t i = 0, off = 0; i < taps_n; ++i) { pos0_at_tdo[i] = off; off += (*taps)[i].ir_length; }
        for (std::uint32_t i = taps_n, off = 0; i-- > 0;)   { pos0_at_tdi[i] = off; off += (*taps)[i].ir_length; }

        const bool tdo = capture_matches(captured->ir, *taps, pos0_at_tdo);
        const bool tdi = capture_matches(captured->ir, *taps, pos0_at_tdi);
        std::printf("IR capture (%u bits): ", ir_total);
        for (std::uint32_t b = 0; b < ir_total; ++b)
            std::putchar(captured->ir.bit(b) ? '1' : '0');
        std::printf("  (first bit out of TDO on the left)\n");
        if (tdo && tdi)
            std::printf("TapInfo::position 0: can't tell on this chain (both orders fit)\n");
        else if (tdo)
            std::printf("TapInfo::position 0 is nearest TDO\n");
        else if (tdi)
            std::printf("TapInfo::position 0 is nearest TDI\n");
        else
            std::printf("IR capture matches neither order: IR lengths or capture values differ from the scan\n");

        bool bypass_ok = true;
        for (std::uint32_t b = 0; b < taps_n; ++b)
            bypass_ok = bypass_ok && !captured->dr.bit(b);
        std::printf("DR in BYPASS: %u bit(s), %s\n", taps_n, bypass_ok ? "all captured 0 (ok)" : "unexpected 1s");
        return {};
    });
    if (!done) {
        std::fprintf(stderr, "%s\n", done.error().message().c_str());
        return 1;
    }
    return 0;
}
