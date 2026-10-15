// UB: the const char* fields of AJI_HARDWARE (hw_name, port, device_name,
// server) are not copies. aji_get_hardware2 slices the library's chain object
// into the caller's array (`*hardware_list++ = *(*j).m_chain;`,
// jtag_client_chain.cpp:143), so the pointers alias storage owned by that
// chain object and die with it.
//
// EXPECT: AddressSanitizer: heap-use-after-free
// EXPECT: in main
// EXPECT: 02_dangling_hardware_strings.cpp:
// ASAN_OPTIONS: alloc_dealloc_mismatch=0

#include "aji_ub_common.hpp"

#include <cstring>

int main()
{
    ub::check(aji_add_remote_server("localhost:1", "pw", true), "aji_add_remote_server(temporary)");

    AJI_HARDWARE hw[16];
    ub::get_hardware(hw, 16);
    const char * server = hw[0].server;
    std::printf("server before remove: %s\n", server);

    ub::check(aji_remove_hardware(hw[0].chain_id), "aji_remove_hardware");

    std::printf("server after remove: %zu chars\n", std::strlen(server));  // UAF here
    return 0;
}
