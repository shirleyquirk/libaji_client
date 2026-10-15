#include "aji/error.hpp"

#include <string>

namespace aji {

std::string_view to_string(Errc e) noexcept
{
    switch (e) {
    case Errc::ok:                             return "ok";
    case Errc::failure:                        return "failure";
    case Errc::timeout:                        return "timeout";
    case Errc::unknown_hardware:               return "unknown_hardware";
    case Errc::invalid_chain_id:               return "invalid_chain_id";
    case Errc::locked:                         return "locked";
    case Errc::not_locked:                     return "not_locked";
    case Errc::chain_in_use:                   return "chain_in_use";
    case Errc::no_devices:                     return "no_devices";
    case Errc::chain_not_configured:           return "chain_not_configured";
    case Errc::bad_tap_position:               return "bad_tap_position";
    case Errc::device_doesnt_match:            return "device_doesnt_match";
    case Errc::ir_length_error:                return "ir_length_error";
    case Errc::device_not_configured:          return "device_not_configured";
    case Errc::chains_claimed:                 return "chains_claimed";
    case Errc::invalid_open_id:                return "invalid_open_id";
    case Errc::invalid_parameter:              return "invalid_parameter";
    case Errc::bad_tap_state:                  return "bad_tap_state";
    case Errc::too_many_devices:               return "too_many_devices";
    case Errc::ir_multiple:                    return "ir_multiple";
    case Errc::bad_sequence:                   return "bad_sequence";
    case Errc::instruction_claimed:            return "instruction_claimed";
    case Errc::mode_not_available:             return "mode_not_available";
    case Errc::invalid_dummy_bits:             return "invalid_dummy_bits";
    case Errc::file_error:                     return "file_error";
    case Errc::net_down:                       return "net_down";
    case Errc::server_error:                   return "server_error";
    case Errc::no_memory:                      return "no_memory";
    case Errc::bad_port:                       return "bad_port";
    case Errc::port_in_use:                    return "port_in_use";
    case Errc::bad_hardware:                   return "bad_hardware";
    case Errc::bad_jtag_chain:                 return "bad_jtag_chain";
    case Errc::server_active:                  return "server_active";
    case Errc::not_permitted:                  return "not_permitted";
    case Errc::hardware_disabled:              return "hardware_disabled";
    case Errc::hierarchical_hub_not_supported: return "hierarchical_hub_not_supported";
    case Errc::unimplemented:                  return "unimplemented";
    case Errc::internal_error:                 return "internal_error";
    case Errc::no_hubs:                        return "no_hubs";
    case Errc::too_many_hubs:                  return "too_many_hubs";
    case Errc::no_matching_nodes:              return "no_matching_nodes";
    case Errc::too_many_matching_nodes:        return "too_many_matching_nodes";
    case Errc::too_many_hierarchies:           return "too_many_hierarchies";
    case Errc::chains_in_use:                  return "chains_in_use";
    case Errc::no_match:                       return "no_match";
    case Errc::ambiguous_match:                return "ambiguous_match";
    case Errc::lock_handed_away:               return "lock_handed_away";
    case Errc::lock_lost:                      return "lock_lost";
    case Errc::not_flushed:                    return "not_flushed";
    case Errc::buffer_too_small:               return "buffer_too_small";
    }
    return "unknown";
}

namespace {

class Category final : public std::error_category {
public:
    const char* name() const noexcept override { return "aji"; }
    std::string message(int ev) const override { return std::string(to_string(static_cast<Errc>(ev))); }
};

} // namespace

const std::error_category& category() noexcept
{
    static const Category instance;
    return instance;
}

std::string Error::message() const
{
    std::string out = call;
    out += ": ";
    out += to_string(code);
    if (!info.empty()) {
        out += " (";
        out += info;
        out += ')';
    }
    return out;
}

} // namespace aji
