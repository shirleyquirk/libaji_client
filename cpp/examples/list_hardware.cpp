// Example: finding hardware.
//
//   list_hardware
//
// Shows: aji::scan, HardwareInfo, dummy entries, aji::find with your own
// predicate, find_by_persistent_id, and the management guard (why a rescan
// fails while you still hold a Chain).
#include "aji/aji.hpp"

#include <cstdio>

using namespace std::chrono_literals;

int main()
{
    // Print the library's own messages (connection problems etc.).
    aji::on_output([](aji::OutputLevel level, std::string_view line) {
        if (level <= aji::OutputLevel::warning)
            std::fprintf(stderr, "aji: %.*s\n", int(line.size()), line.data());
    });

    std::uint32_t remembered_id = 0;

    // 1. Everything the local and remote servers report.
    {
        auto chains = aji::scan(5s);
        if (!chains) {
            std::fprintf(stderr, "scan failed: %s\n", chains.error().message().c_str());
            return 1;
        }
        for (const aji::Chain& chain : *chains) {
            const aji::HardwareInfo& h = chain.info();
            if (h.is_dummy()) {
                // A server that is unreachable or still connecting.
                std::printf("  (no hardware) %s\n", h.display_name().c_str());
                continue;
            }
            std::printf("  %-40s id=%08X server=%s version=%s\n", h.display_name().c_str(), h.persistent_id,
                        h.server.value_or("local").c_str(), h.server_version.c_str());
            if (remembered_id == 0)
                remembered_id = h.persistent_id;
        }

        // 2. While `chains` is alive, management calls refuse to run: they
        //    could make the library free the chains we're holding.
        auto again = aji::scan(1s);
        std::printf("rescan while holding chains: %s\n",
                    again ? "ok?!" : again.error().message().c_str());   // "aji::scan: chains_in_use"
    }   // chains released here

    // 3. Pick exactly one chain with your own filter. no_match and
    //    ambiguous_match are ordinary errors.
    {
        auto usb = aji::find([](const aji::HardwareInfo& h) { return !h.is_dummy() && h.hw_name.starts_with("USB"); });
        if (usb)
            std::printf("the USB cable: %s\n", usb->info().display_name().c_str());
        else
            std::printf("not exactly one USB cable: %s\n", usb.error().message().c_str());
    }   // dropped before the next management call

    // 4. A persistent id survives reboots: store it in a config file, find
    //    the same cable next time.
    if (remembered_id != 0) {
        auto again = aji::find_by_persistent_id(remembered_id, 2s);
        std::printf("by persistent id %08X: %s\n", remembered_id,
                    again ? again->info().display_name().c_str() : again.error().message().c_str());
    }
    return 0;
}
