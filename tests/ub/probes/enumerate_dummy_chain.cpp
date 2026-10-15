// Exploration probe (not a pass/fail test): what aji_get_hardware2 reports
// for an unreachable temporary remote server, over several rescans.
//
// Observed with the sanitized build (see ../README.md):
//   - exactly one chain, features = AJI_FEATURE_DUMMY (0x1), hw_name NULL,
//     server "localhost:1"
//   - the same AJI_CHAIN_ID on every rescan: a dummy chain is not recreated
//     while the server stays unreachable
// This is the setup the numbered tests rely on. It also shows that dummy
// entries are real chains, so the wrapper has to filter them explicitly
// (DESIGN.md §3).
//
// Use "localhost:1", not "127.0.0.1:1": a dotted IP triggers the overflow in
// test 03.
//
// Build/run like the numbered tests; ASAN_OPTIONS=detect_leaks=0 because this
// probe doesn't free the version strings.
#include "aji_ub_common.hpp"

int main()
{
    ub::check(aji_add_remote_server("localhost:1", "pw", true), "aji_add_remote_server(temporary)");
    for (int round = 0; round < 3; ++round)
    {
        DWORD n = 8;
        AJI_HARDWARE hw[8];
        char * versions[8] = {};
        AJI_ERROR e = aji_get_hardware2(&n, hw, versions, 2000);
        std::printf("round %d: get=%d n=%u\n", round, static_cast<int>(e), n);
        for (DWORD i = 0; i < n && i < 8; ++i)
            std::printf("  [%u] chain=%p hw_name=%s server=%s features=%#x\n", i,
                        static_cast<void *>(hw[i].chain_id),
                        hw[i].hw_name ? hw[i].hw_name : "(null)",
                        hw[i].server ? hw[i].server : "(null)",
                        hw[i].features);
    }
    return 0;
}
