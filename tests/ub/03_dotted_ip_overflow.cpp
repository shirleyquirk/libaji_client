// Library bug found while writing these tests (not a misuse of the API).
//
// TCPCLIENT::lookup allocates 4 bytes for an IPv4 address and stores an
// `unsigned long` into it (jtag_tcpclient.cpp:126 and :133). On LP64
// Linux that is an 8-byte store into a 4-byte buffer. Any server given as a
// dotted IPv4 address ("192.168.1.5:1309") hits it on the first
// aji_get_hardware2. Host names and "localhost" go through other branches.
//
// Consequence for the wrapper: until the library is fixed, resolve server
// names ourselves or document the restriction.
//
// EXPECT: AddressSanitizer: heap-buffer-overflow
// EXPECT: in TCPCLIENT::lookup

#include "aji_ub_common.hpp"

int main()
{
    ub::check(aji_add_remote_server("127.0.0.1:1", "pw", true), "aji_add_remote_server(127.0.0.1)");

    AJI_HARDWARE hw[16];
    DWORD n = 16;
    char * versions[16] = {};
    ub::check(aji_get_hardware2(&n, hw, versions, 2000), "aji_get_hardware2");  // overflow inside
    return 0;
}
