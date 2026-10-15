// Shared setup for the UB demonstrations in this directory.
//
// No JTAG hardware or jtagd is needed. Each test registers a *temporary*
// remote server at localhost:1 (nothing listens there), with the local server
// disabled by run.sh (QUARTUS_JTAG_CLIENT_NO_LOCAL_SERVER=1). The library then
// reports one dummy chain ("Unable to connect") for that server, which is a
// real AJI_CHAIN_JS object owned by the library -- enough to show the
// lifetime problems.
#pragma once

#include "aji.h"

#include <cstdio>
#include <cstdlib>

namespace ub {

inline void check(AJI_ERROR e, const char * what)
{
    std::printf("%-40s -> %d\n", what, static_cast<int>(e));
    std::fflush(stdout);
}

// Calls aji_get_hardware2 and frees the server-version strings, which the
// caller owns (new[] at jtag_client_chain.cpp:150/155).
inline DWORD get_hardware(AJI_HARDWARE * hw, DWORD space)
{
    char * versions[16] = {};
    DWORD n = space;
    check(aji_get_hardware2(&n, hw, versions, 2000), "aji_get_hardware2");
    for (DWORD i = 0; i < n && i < space; ++i)
        delete[] versions[i];
    if (n == 0)
    {
        std::printf("no chains reported; test setup failed\n");
        std::exit(2);
    }
    return n;
}

} // namespace ub
