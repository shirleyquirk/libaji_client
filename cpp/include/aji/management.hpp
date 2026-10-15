// Management functions: everything that can make the library free chain
// objects, their strings, or whole server connections (FINDINGS.md §2.4).
//
// Each returns Errc::chains_in_use, without calling the library, while any
// Chain, held ChainLock or Open is alive. That turns what would be a
// use-after-free into an error. The library frees things only inside these calls; there are no
// background threads, so holding a Chain is safe between them.
//
// They are also serialised against each other (one process-wide mutex): the
// library's server list is global and unsynchronised.
#pragma once

#include "aji/chain.hpp"
#include "aji/error.hpp"
#include "aji/types.hpp"

#include <chrono>
#include <concepts>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aji {

using namespace std::chrono_literals;

// ---- finding hardware ----

// All chains on the local server and the configured remote servers
// (aji_get_hardware2), dummy entries included (HardwareInfo::is_dummy).
// Servers still connecting after `timeout` show up as dummy entries; that is
// not an error.
//
// A timeout of 0 is raised to 1 ms. With QUARTUS_JTAG_CLIENT_NO_LOCAL_SERVER
// set, the library crashes if it finds no chain at all (FINDINGS.md §4):
// timeout 0 before any server was contacted is one way to get there; having
// no remote servers configured is the other, and that one we can't avoid.
expected<std::vector<Chain>> scan(std::chrono::milliseconds timeout = 2s);

// scan(), then the one chain whose info matches `pred`.
// Errc::no_match / Errc::ambiguous_match otherwise.
template<std::predicate<const HardwareInfo&> P>
expected<Chain> find(P&& pred, std::chrono::milliseconds timeout = 2s)
{
    auto all = scan(timeout);
    if (!all)
        return unexpected(all.error());
    std::optional<Chain> hit;
    bool ambiguous = false;
    for (Chain& c : *all) {
        if (!pred(c.info()))
            continue;
        if (hit)
            ambiguous = true;
        else
            hit.emplace(std::move(c));
    }
    if (ambiguous)
        return unexpected(Error{Errc::ambiguous_match, "aji::find", {}});
    if (!hit)
        return unexpected(Error{Errc::no_match, "aji::find", {}});
    return std::move(*hit);
}

// aji_find_hardware by user-visible name ("USB-Blaster [1-1]",
// "USB-Blaster on host [1-1]", ...). HardwareInfo::server_version is empty.
expected<Chain> find_by_name(std::string_view name, std::chrono::milliseconds timeout = 2s);

// aji_find_hardware by persistent id (HardwareInfo::persistent_id).
expected<Chain> find_by_persistent_id(std::uint32_t id, std::chrono::milliseconds timeout = 2s);

// ---- changing the hardware list ----

expected<void> add_hardware(const HardwareSpec& spec);

// aji_remove_hardware. Takes the last handle to the chain: fails with
// chains_in_use if any other Chain or Open is alive. For a chain on a remote
// server this removes the connection to that server.
// Removing a *temporary* remote server triggers a new[]/delete mismatch
// inside the library (FINDINGS.md §4); harmless with glibc, reported by ASan.
expected<void> remove(Chain chain);

enum class Persist { permanent, temporary };

// aji_add_remote_server. A dotted IPv4 host ("192.168.1.5:1309") overflows a
// 4-byte buffer inside the library on 64-bit Linux (FINDINGS.md §4); prefer
// a host name.
expected<void> add_remote_server(const std::string& host, const std::string& password,
                                 Persist persist = Persist::permanent);

expected<void> enable_remote_clients(bool enable, const std::string& password);
expected<bool> remote_clients_enabled();

// ---- the user device database ----

expected<void>                    define_device(const DeviceSpec& device);
expected<void>                    undefine_device(const DeviceSpec& device);
expected<std::vector<DeviceSpec>> defined_devices();

// ---- not guarded: safe at any time ----

enum class OutputLevel : std::uint32_t { error = 0, warning = 1, info = 2, debug = 3 };

// aji_register_output_callback. May be called from another thread.
// An empty function unregisters.
void on_output(std::function<void(OutputLevel, std::string_view)> fn);

// aji_get_error_info(): the library's last error text (process-wide).
std::string last_error_info();

} // namespace aji
