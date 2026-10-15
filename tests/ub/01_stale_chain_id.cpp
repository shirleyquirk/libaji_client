// UB: an AJI_CHAIN_ID handed out by aji_get_hardware2 is a raw pointer to a
// library-owned AJI_CHAIN_JS. When the library deletes that object, every
// aji_* entry point that takes the id makes a virtual call through it with no
// validity check (e.g. jtag_client_aji.cpp:199 for aji_print_hardware_name),
// so a stale id is a use-after-free, not an AJI_INVALID_CHAIN_ID error.
//
// Here the chain is deleted by aji_remove_hardware on a remote-server chain
// (jtag_client_chain.cpp:773 -> AJI_CLIENT::remove_remote_server -> delete
// this -> ~AJI_CLIENT deletes its chains, jtag_client_link.cpp:643). The same
// delete happens on a rescan when a cable disappears
// (AJI_CLIENT::refresh_hardware, jtag_client_link.cpp:1446-1456), which needs
// real hardware to reproduce.
//
// EXPECT: AddressSanitizer: heap-use-after-free
// EXPECT: #0 
// EXPECT: in aji_print_hardware_name
// ASAN_OPTIONS: alloc_dealloc_mismatch=0

#include "aji_ub_common.hpp"

int main()
{
    ub::check(aji_add_remote_server("localhost:1", "pw", true), "aji_add_remote_server(temporary)");

    AJI_HARDWARE hw[16];
    ub::get_hardware(hw, 16);
    AJI_CHAIN_ID chain = hw[0].chain_id;

    ub::check(aji_remove_hardware(chain), "aji_remove_hardware");   // frees *chain

    char name[128];
    ub::check(aji_print_hardware_name(chain, name, sizeof name),     // UAF here
              "aji_print_hardware_name(stale id)");
    return 0;
}
