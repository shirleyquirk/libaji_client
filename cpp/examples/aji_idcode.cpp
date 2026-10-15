// aji_idcode: a self-checking hardware test of the lock machinery.
//
//   aji_idcode <chain-name-substring> [idcode-instruction=6]
//
// For every TAP whose IR length is known, it reads IDCODE through the
// wrapper and compares it with the IDCODE the chain scan reported:
//   1. chain -> TAP -> chain (hand_to / hand_back), once per pack style
//   2. TAP -> next TAP -> chain (Session::hand_to, aji_unlock_lock), if there
//      are two usable TAPs
//   3. Open::lock (aji_lock) after the chain lock has been released
// and checks the lock rules on the way: ChainLock empty while handed off,
// held again after hand_back, Errc::wrong_thread from another thread.
// Prints PASS/FAIL per check; exits non-zero if anything failed.
//
// The IDCODE instruction is device-specific: 6 is right for Intel FPGAs (10-bit IR).
// TAPs that don't implement it at that value will FAIL; pass another value or
// ignore those lines.
#include "aji/aji.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

int failures = 0;

void report(bool ok, const char* what, std::uint32_t pos, const std::string& detail)
{
    std::printf("%s  tap %u  %-30s %s\n", ok ? "PASS" : "FAIL", pos, what, detail.c_str());
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

// IR <- instruction, then 32 bits of DR, flushed.
template<aji::Kind K>
aji::expected<aji::Bits> read_idcode(aji::Session<K>& s, std::uint32_t instruction)
{
    (void)s.ir(instruction);
    auto dr = s.dr({.length = 32, .read_length = 32});
    if (auto f = s.flush(); !f)
        return aji::unexpected(f.error());
    return dr.get();
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

struct Usable {
    aji::TapInfo              tap;
    aji::Open<aji::Kind::tap> open;
};

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

    auto cl = chain->lock(10s);
    if (!cl) {
        std::fprintf(stderr, "lock: %s\n", cl.error().message().c_str());
        return 1;
    }

    // The lock belongs to this thread.
    {
        bool refused = false;
        std::thread([&] {
            auto r = cl->taps();
            refused = !r && r.error().code == aji::Errc::wrong_thread;
        }).join();
        report(refused, "other thread: wrong_thread", 0, "");
    }

    auto taps = cl->taps();
    if (!taps) {
        std::fprintf(stderr, "taps: %s\n", taps.error().message().c_str());
        return 1;
    }

    std::vector<Usable> usable;
    for (const aji::TapInfo& tap : *taps) {
        if (!tap.ir_length_known()) {
            std::printf("skip  tap %u  IR length unknown\n", tap.position);
            continue;
        }
        auto open = cl->open_tap(tap.position, aji::ClaimSet{}.add(aji::ClaimType::ir_shared, instruction),
                                 "aji_idcode");
        if (!open) {
            report(false, "open_tap", tap.position, open.error().message());
            continue;
        }
        usable.push_back({tap, std::move(*open)});
    }

    // 1. chain -> TAP -> chain, per pack style.
    const std::pair<aji::Pack, const char*> packs[] = {
        {aji::Pack::never, "hand_to, Pack::never"},
        {aji::Pack::automatic, "hand_to, Pack::automatic"},
        {aji::Pack::manual, "hand_to, Pack::manual"},
    };
    for (auto& [tap, open] : usable) {
        for (auto [pack, what] : packs) {
            auto s = cl->hand_to(open, pack);
            if (!s) {
                report(false, what, tap.position, s.error().message());
                continue;
            }
            report(!*cl, "ChainLock empty while handed off", tap.position, "");
            check(what, tap, read_idcode(*s, instruction));
            auto back = s->hand_back();
            if (!back) {
                report(false, "hand_back", tap.position, back.error().message());
                return 1;   // the lock is gone; nothing more to test
            }
            *cl = std::move(*back);
            report(bool(*cl) && !*s, "hand_back: chain held, session empty", tap.position, "");
        }
    }

    // 2. TAP -> next TAP (aji_unlock_lock) -> chain.
    if (usable.size() >= 2) {
        auto& [tap0, open0] = usable[0];
        auto& [tap1, open1] = usable[1];
        auto s0 = cl->hand_to(open0, aji::Pack::manual);
        if (!s0) {
            report(false, "hand_to tap", tap0.position, s0.error().message());
        } else {
            check("open->open: first", tap0, read_idcode(*s0, instruction));
            auto s1 = s0->hand_to(open1);
            if (!s1) {
                report(false, "Session::hand_to", tap1.position, s1.error().message());
            } else {
                check("open->open: second", tap1, read_idcode(*s1, instruction));
                auto back = s1->hand_back();
                if (!back) {
                    report(false, "hand_back after open->open", tap1.position, back.error().message());
                    return 1;
                }
                *cl = std::move(*back);
            }
        }
    }

    // 3. Release the chain; each open takes the lock itself (aji_lock).
    if (auto u = cl->unlock(); !u)
        report(false, "ChainLock::unlock", 0, u.error().message());
    for (auto& [tap, open] : usable) {
        auto s = open.lock(10s, aji::Pack::manual);
        if (!s) {
            report(false, "Open::lock", tap.position, s.error().message());
            continue;
        }
        check("Open::lock, manual", tap, read_idcode(*s, instruction));
        if (auto u = s->unlock(); !u)
            report(false, "Session::unlock", tap.position, u.error().message());
    }

    std::printf("%s (%d failure%s)\n", failures == 0 ? "OK" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
