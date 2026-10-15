// Example: chain parameters. Read (and optionally set) the JTAG clock.
//
//   jtag_clock <cable-substring>              print JtagClock and JtagClockAutoAdjust
//   jtag_clock <cable-substring> 6000000      set JtagClock to 6 MHz
//
// Same as `jtagconfig --getparam/--setparam`. Shows: ChainLock::parameter,
// set_parameter, and a callback that returns expected<void>.
#include "aji/aji.hpp"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

using namespace std::chrono_literals;

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <cable-substring> [new-clock-hz]\n", argv[0]);
        return 2;
    }
    const std::string cable = argv[1];
    std::optional<std::uint32_t> new_clock;
    if (argc > 2)
        new_clock = std::uint32_t(std::strtoul(argv[2], nullptr, 0));

    auto chain = aji::find([&](const aji::HardwareInfo& h) {
        return !h.is_dummy() && h.display_name().find(cable) != std::string::npos;
    });
    if (!chain) {
        std::fprintf(stderr, "%s\n", chain.error().message().c_str());
        return 1;
    }

    auto done = chain->locked(10s, [&](auto cl) -> aji::expected<void> {
        if (new_clock) {
            if (auto set = cl.set_parameter("JtagClock", *new_clock); !set)
                return set;   // "aji_set_parameter: unimplemented" if the cable has no such parameter
        }
        for (const char* name : {"JtagClock", "JtagClockAutoAdjust"}) {
            auto value = cl.parameter(name);
            if (value)
                std::printf("%-20s %u\n", name, *value);
            else
                std::printf("%-20s (%s)\n", name, value.error().message().c_str());
        }
        return {};
    });
    if (!done) {
        std::fprintf(stderr, "%s\n", done.error().message().c_str());
        return 1;
    }
    return 0;
}
