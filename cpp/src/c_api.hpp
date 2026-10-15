// Private to the wrapper's .cpp files: includes aji.h and converts between
// its types and the wrapper's. CMake sets the platform macros aji.h needs.
#pragma once

#include "aji.h"

#include "aji/error.hpp"
#include "aji/types.hpp"

#include <cstring>
#include <optional>
#include <string>
#include <vector>

// The public headers use <cstdint> names and their own mirrors of a few
// structs; check they really match.
static_assert(std::is_same_v<DWORD, std::uint32_t>);
static_assert(std::is_same_v<BYTE, std::uint8_t>);
static_assert(std::is_same_v<QWORD, std::uint64_t>);
static_assert(AJI_MAX_HIERARCHICAL_HUB_DEPTH == aji::max_hub_depth);
static_assert(int(AJI_TOO_MANY_HIERARCHIES) == int(aji::Errc::too_many_hierarchies));
static_assert(int(AJI_HARDWARE_DISABLED) == int(aji::Errc::hardware_disabled));
static_assert(int(AJI_CLAIM_OVERLAY_WEAK) == int(aji::ClaimType::overlay_weak));
static_assert(int(AJI_PACK_STREAM) == int(aji::Pack::stream));
static_assert(int(AJI_DR_NO_RESTORE) == int(aji::DrFlag::no_restore));

namespace aji::detail {

inline Error from_aji(AJI_ERROR e, const char* call)
{
    const char* info = aji_get_error_info();
    return Error{static_cast<Errc>(e), call, info ? info : ""};
}

inline expected<void> check(AJI_ERROR e, const char* call)
{
    if (e != AJI_NO_ERROR)
        return unexpected(from_aji(e, call));
    return {};
}

inline std::optional<std::string> opt(const char* s)
{
    if (s == nullptr)
        return std::nullopt;
    return std::string(s);
}

inline HardwareInfo info_from(const AJI_HARDWARE& hw, const char* server_version)
{
    HardwareInfo info;
    info.persistent_id  = hw.persistent_id;
    info.hw_name        = hw.hw_name ? hw.hw_name : "";
    info.port           = opt(hw.port);
    info.device_name    = opt(hw.device_name);
    info.server         = opt(hw.server);
    info.chain_type     = static_cast<ChainType>(hw.chain_type);
    info.features       = Features(hw.features);
    info.server_version = server_version ? server_version : "";
    return info;
}

inline TapInfo tap_from(const AJI_DEVICE& d, std::uint32_t position)
{
    return TapInfo{
        .position    = position,
        .idcode      = d.device_id,
        .idcode_mask = d.mask,
        .ir_length   = d.instruction_length,
        .features    = DeviceFeatures(d.features),
        .name        = opt(d.device_name),
    };
}

inline DeviceSpec device_from(const AJI_DEVICE& d)
{
    return DeviceSpec{
        .idcode      = d.device_id,
        .idcode_mask = d.mask,
        .ir_length   = d.instruction_length,
        .features    = DeviceFeatures(d.features),
        .name        = d.device_name ? d.device_name : "",
    };
}

// The AJI_DEVICE points into `spec`; keep `spec` alive while it is used.
inline AJI_DEVICE device_to(const DeviceSpec& spec)
{
    return AJI_DEVICE{
        .device_id          = spec.idcode,
        .mask               = spec.idcode_mask,
        .instruction_length = spec.ir_length,
        .features           = spec.features.bits(),
        .device_name        = spec.name.c_str(),
    };
}

inline std::vector<AJI_CLAIM2> claims_to(const ClaimSet& set)
{
    std::vector<AJI_CLAIM2> out;
    for (const Claim& c : set.claims())
        out.push_back(AJI_CLAIM2{static_cast<AJI_CLAIM_TYPE>(c.type), 0, c.value});
    return out;
}

inline AJI_HIER_ID hier_to(const HierId& h)
{
    AJI_HIER_ID out{};
    out.idcode = h.idcode;
    out.position_n = h.position_n;
    for (std::size_t i = 0; i < max_hub_depth; ++i)
        out.positions[i] = h.positions[i];
    return out;
}

inline HierId hier_from(const AJI_HIER_ID& h)
{
    HierId out;
    out.idcode = h.idcode;
    out.position_n = h.position_n;
    for (std::size_t i = 0; i < max_hub_depth; ++i)
        out.positions[i] = h.positions[i];
    return out;
}

inline HubInfo hub_from(const AJI_HUB_INFO& h)
{
    HubInfo out;
    for (std::size_t i = 0; i < max_hub_depth; ++i) {
        out.bridge_idcode[i] = h.bridge_idcode[i];
        out.hub_idcode[i] = h.hub_idcode[i];
    }
    return out;
}

} // namespace aji::detail
