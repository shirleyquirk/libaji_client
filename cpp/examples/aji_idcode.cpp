// aji_idcode: a self-checking hardware test of the session machinery.
//
//   aji_idcode <chain-name-substring> [idcode-instruction=6]
//
// For every TAP whose IR length is known, it reads IDCODE through the
// wrapper and compares it with the IDCODE the chain scan reported:
//   1. through ChainLock::with, once per pack style (never, automatic, manual)
//   2. through Open::locked (aji_lock) after the chain lock has been released
// Prints PASS/FAIL per check; exits non-zero if anything failed.
//
// The IDCODE instruction is device-specific: 6 is right for Intel FPGAs (10-bit IR).
// TAPs that don't implement it at that value will FAIL; pass another value or
// ignore those lines.
#include "aji/aji.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

int failures = 0;

void report(bool ok, const char* what, std::uint32_t pos, const std::string& detail)
{
    std::printf("%s  tap %u  %-24s %s\n", ok ? "PASS" : "FAIL", pos, what, detail.c_str());
    if (!ok)
        ++failures;
}

std::uint32_t to_u32(const aji::Bits& bits)
{
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4 && i < bits.bytes.size(); ++i)
        v |= std::uint32_t(bits.bytes[i]) << (8 * i);
    return v;
}

std::string hex(std::uint32_t v)
{
    char b[16];
    std::snprintf(b, sizeof b, "%08X", v);
    return b;
}

// Reads IDCODE in one session: IR <- instruction, then 32 bits of DR.
auto read_idcode(std::uint32_t instruction)
{
    return [instruction](auto s) {
        (void)s.ir(instruction);
        return s.dr({.length = 32, .read_length = 32});
    };
}

void check(const char* what, const aji::TapInfo& tap, const aji::expected<aji::Bits>& got)
{
    if (!got) {
        report(false, what, tap.position, got.error().message());
        return;
    }
    const std::uint32_t mask = ~tap.idcode_mask;
    const std::uint32_t v = to_u32(*got);
    report((v & mask) == (tap.idcode & mask), what, tap.position,
           "read " + hex(v) + ", scan said " + hex(tap.idcode));
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <chain-name-substring> [idcode-instruction=6]\n", argv[0]);
        return 2;
    }
    const std::string filter = argv[1];
    const std::uint32_t instruction = argc > 2 ? std::uint32_t(std::strtoul(argv[2], nullptr, 0)) : 6;

    auto chain = aji::find([&](const aji::HardwareInfo& h) {
        return !h.is_dummy() && h.display_name().find(filter) != std::string::npos;
    }, 5s);
    if (!chain) {
        std::fprintf(stderr, "find: %s\n", chain.error().message().c_str());
        return 1;
    }
    std::printf("chain: %s\n", chain->info().display_name().c_str());

    // Opens outlive the chain lock: step 2 uses them afterwards.
    std::vector<std::pair<aji::TapInfo, aji::Open<aji::Kind::tap>>> opened;

    auto step1 = chain->locked(10s, [&](auto cl) -> aji::expected<void> {
        auto taps = cl.taps();
        if (!taps)
            return aji::unexpected(taps.error());

        for (const aji::TapInfo& tap : *taps) {
            if (!tap.ir_length_known()) {
                std::printf("skip  tap %u  IR length unknown\n", tap.position);
                continue;
            }
            auto open = cl.open_tap(tap.position, aji::ClaimSet{}.add(aji::ClaimType::ir_shared, instruction),
                                    "aji_idcode");
            if (!open) {
                report(false, "open_tap", tap.position, open.error().message());
                continue;
            }
            check("with, Pack::never", tap, cl.with(*open, aji::Pack::never, read_idcode(instruction)));
            check("with, Pack::automatic", tap, cl.with(*open, aji::Pack::automatic, read_idcode(instruction)));
            check("with, Pack::manual", tap, cl.with(*open, aji::Pack::manual, read_idcode(instruction)));
            opened.emplace_back(tap, std::move(*open));
        }
        return {};
    });
    if (!step1)
        report(false, "chain.locked", 0, step1.error().message());

    // The chain lock is released; each open takes the lock itself (aji_lock).
    for (auto& [tap, open] : opened)
        check("Open::locked, manual", tap, open.locked(10s, aji::Pack::manual, read_idcode(instruction)));

    std::printf("%s (%d failure%s)\n", failures == 0 ? "OK" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
