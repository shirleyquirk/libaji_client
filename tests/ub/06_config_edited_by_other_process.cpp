// UB caused by a *different process*: editing the shared JTAG client config
// file (~/.jtag.conf by default, or $QUARTUS_JTAG_CLIENT_CONFIG) frees our
// chain objects on our next call that rescans.
//
// The config file is re-read whenever its mtime/ctime changes
// (jtag_configuration_file.cpp:178-182). AJI_CLIENT::update_servers deletes
// every server connection whose RemoteN entry vanished or changed
// (jtag_client_link.cpp:196-223), and ~AJI_CLIENT deletes its chains
// (jtag_client_link.cpp:640-643). This doesn't check whether we have that
// server locked or devices open on it.
//
// update_servers never examines m_link[0] (its loop stops at begin(),
// jtag_client_link.cpp:203). In normal use that slot is the local jtagd. With
// the local server disabled for this test it is Remote1, so the test removes
// Remote2.
//
// The child process here plays `jtagconfig --removeserver` (or the Quartus
// Programmer's Hardware Setup dialog) run by someone else on the same account.
//
// EXPECT: AddressSanitizer: heap-use-after-free
// EXPECT: in aji_print_hardware_name
// EXPECT: freed by thread T0 here:
// EXPECT: in AJI_CLIENT::update_servers

#include "aji_ub_common.hpp"

#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

static void write_config(const char * path, const char * text)
{
    FILE * f = std::fopen(path, "w");
    if (f == nullptr) { std::perror(path); std::exit(2); }
    std::fputs(text, f);
    std::fclose(f);
}

int main()
{
    const char * path = std::getenv("QUARTUS_JTAG_CLIENT_CONFIG");
    if (path == nullptr) { std::puts("run via run.sh"); return 2; }

    // Two persistent remote servers, as `jtagconfig --addserver` would write them.
    const char * one = "Remote1 {\n\tHost = \"localhost:1\";\n\tPassword = \"pw\";\n}\n";
    const char * two = "Remote2 {\n\tHost = \"localhost:2\";\n\tPassword = \"pw\";\n}\n";
    write_config(path, (std::string(one) + two).c_str());

    AJI_HARDWARE hw[16];
    DWORD n = ub::get_hardware(hw, 16);
    AJI_CHAIN_ID chain = nullptr;
    for (DWORD i = 0; i < n; ++i)
        if (hw[i].server != nullptr && std::strcmp(hw[i].server, "localhost:2") == 0)
            chain = hw[i].chain_id;
    if (chain == nullptr) { std::puts("no chain for localhost:2; setup failed"); return 2; }
    std::printf("chain for localhost:2 = %p\n", static_cast<void *>(chain));

    // Another process removes the server from the shared config. The mtime has
    // one-second resolution, so wait for it to differ.
    sleep(1);
    if (fork() == 0)
    {
        write_config(path, one);           // Remote2 removed
        _exit(0);
    }
    wait(nullptr);

    ub::get_hardware(hw, 16);               // our own rescan frees `chain`

    char name[128];
    ub::check(aji_print_hardware_name(chain, name, sizeof name), "aji_print_hardware_name(stale id)");
    return 0;
}
