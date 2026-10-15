// UB without any hardware change: a plain rescan frees the AJI_HARDWARE
// strings of a chain that still exists, under the same AJI_CHAIN_ID.
//
// Every aji_get_hardware2 with timeout > 0 runs refresh_hardware, which calls
// refresh_chain -> AJI_CHAIN_JS::set_strings on chains that are still present
// (jtag_client_link.cpp:1593-1599). set_strings delete[]s the old string
// block whenever any name changed (jtag_client_chain.cpp:651-663). Here the
// name changes because the dummy entry for a remote server goes from
// "Connecting ..." to "Unable to connect" (jtag_client_link.cpp:1408-1430);
// with real hardware the same happens when a cable is renamed or a server
// connects. The chain object, and so its id, survives: only the strings move.
//
// EXPECT: AddressSanitizer: heap-use-after-free
// EXPECT: in main
// EXPECT: 05_strings_freed_by_plain_rescan.cpp:
// ASAN_OPTIONS: alloc_dealloc_mismatch=0

#include "aji_ub_common.hpp"

#include <cstring>

static AJI_HARDWARE scan(DWORD timeout)
{
    AJI_HARDWARE hw[16];
    DWORD n = 16;
    char * versions[16] = {};
    AJI_ERROR e = aji_get_hardware2(&n, hw, versions, timeout);
    std::printf("aji_get_hardware2(timeout=%u) -> %d, n=%u, chain=%p server@%p\n", timeout,
                static_cast<int>(e), n, static_cast<void *>(hw[0].chain_id),
                static_cast<const void *>(hw[0].server));
    for (DWORD i = 0; i < n && i < 16; ++i)
        delete[] versions[i];
    return hw[0];
}

int main()
{
    ub::check(aji_add_remote_server("localhost:1", "pw", true), "aji_add_remote_server(temporary)");

    // Timeout 1 ms: still connecting. (Timeout 0 crashes in this setup, a separate
    // library bug: FINDINGS §4.)
    AJI_HARDWARE first = scan(1);
    AJI_HARDWARE second = scan(2000);   // connection refused: dummy renamed, strings reallocated

    std::printf("same chain id: %s\n", first.chain_id == second.chain_id ? "yes" : "no");
    std::printf("old server string: %zu chars\n", std::strlen(first.server));   // UAF here
    return 0;
}
