// Library bug found while writing these tests (not a misuse of the API).
//
// A temporary server's info block is allocated with new[]
// (AJI_CLIENT::add_temporary_server_internal, jtag_client_link.cpp:442) and
// freed with scalar delete (`delete (char *) info;`,
// jtag_client_link.cpp:679). Removing any temporary remote server is UB.
//
// Tests 01 and 02 pass through this path, which is why run.sh gives them
// ASAN_OPTIONS=alloc_dealloc_mismatch=0.
//
// EXPECT: AddressSanitizer: alloc-dealloc-mismatch
// EXPECT: in AJI_CLIENT::remove_remote_server

#include "aji_ub_common.hpp"

int main()
{
    ub::check(aji_add_remote_server("localhost:1", "pw", true), "aji_add_remote_server(temporary)");

    AJI_HARDWARE hw[16];
    ub::get_hardware(hw, 16);

    ub::check(aji_remove_hardware(hw[0].chain_id), "aji_remove_hardware");   // mismatch inside
    return 0;
}
