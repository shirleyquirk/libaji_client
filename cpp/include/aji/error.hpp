// Errors for the C++ wrapper.
//
// Every fallible call returns aji::expected<T>. An Error carries:
//   - code:  an Errc, which mirrors AJI_ERROR value for value, plus a few
//            wrapper-only codes from 1000 up
//   - call:  the name of the aji_* function (or wrapper step) that failed
//   - info:  aji_get_error_info() captured at the moment of failure; often empty
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <system_error>

namespace aji {

enum class Errc : int {
    // ---- mirrors AJI_ERROR (aji.h); values must not change ----
    ok                         = 0,
    failure                    = 1,
    timeout                    = 2,
    unknown_hardware           = 32,
    invalid_chain_id           = 33,
    locked                     = 34,
    not_locked                 = 35,
    chain_in_use               = 36,
    no_devices                 = 37,
    chain_not_configured       = 38,
    bad_tap_position           = 39,
    device_doesnt_match        = 40,
    ir_length_error            = 41,
    device_not_configured      = 42,
    chains_claimed             = 43,
    invalid_open_id            = 44,
    invalid_parameter          = 45,
    bad_tap_state              = 46,
    too_many_devices           = 47,
    ir_multiple                = 48,
    bad_sequence               = 49,
    instruction_claimed        = 50,
    mode_not_available         = 51,
    invalid_dummy_bits         = 52,
    file_error                 = 80,
    net_down                   = 81,
    server_error               = 82,
    no_memory                  = 83,
    bad_port                   = 84,
    port_in_use                = 85,
    bad_hardware               = 86,
    bad_jtag_chain             = 87,
    server_active              = 88,
    not_permitted              = 89,
    hardware_disabled          = 90,
    hierarchical_hub_not_supported = 125,
    unimplemented              = 126,
    internal_error             = 127,
    no_hubs                    = 256,
    too_many_hubs              = 257,
    no_matching_nodes          = 258,
    too_many_matching_nodes    = 259,
    too_many_hierarchies       = 260,

    // ---- wrapper-only ----
    // A management function was called while a Chain, ChainLock or Open is
    // alive. The library could free the objects they point into
    // (docs/cpp-wrapper/FINDINGS.md §2.4).
    chains_in_use              = 1000,
    no_match                   = 1001,  // find*: nothing matched
    ambiguous_match            = 1002,  // find*: more than one matched
    lock_handed_away           = 1003,  // ChainLock used while one of its opens holds the lock
    lock_lost                  = 1004,  // the lock could not be handed back or re-taken
    not_flushed                = 1005,  // Pending::get() before its batch was flushed
    buffer_too_small           = 1006,  // a BitsView shorter than the length it claims
};

const std::error_category& category() noexcept;

inline std::error_code make_error_code(Errc e) noexcept
{
    return {static_cast<int>(e), category()};
}

struct Error {
    Errc        code = Errc::failure;
    const char* call = "";      // static string: the failing aji_* call or wrapper step
    std::string info;           // aji_get_error_info() at the time, if any

    std::error_code error_code() const noexcept { return make_error_code(code); }
    std::string     message() const;            // "aji_lock_chain: chain_in_use (...info...)"
};

template<class T>
using expected = std::expected<T, Error>;

using unexpected = std::unexpected<Error>;

std::string_view to_string(Errc) noexcept;

} // namespace aji

template<>
struct std::is_error_code_enum<aji::Errc> : std::true_type {};
