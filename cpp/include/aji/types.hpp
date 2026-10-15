// Plain value types: copyable, own all their data, refer to no library object.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace aji {

// ---------------------------------------------------------------------------
// Flag sets

template<class E>
class Flags {
public:
    using bits_type = std::underlying_type_t<E>;
    constexpr Flags() = default;
    constexpr Flags(E e) : bits_(static_cast<bits_type>(e)) {}
    constexpr explicit Flags(bits_type b) : bits_(b) {}
    constexpr bool has(E e) const { return (bits_ & static_cast<bits_type>(e)) == static_cast<bits_type>(e); }
    constexpr bits_type bits() const { return bits_; }
    constexpr Flags operator|(Flags o) const { return Flags(bits_type(bits_ | o.bits_)); }
    constexpr bool operator==(const Flags&) const = default;
private:
    bits_type bits_ = 0;
};

template<class E> requires std::is_enum_v<E>
constexpr Flags<E> operator|(E a, E b) { return Flags<E>(a) | Flags<E>(b); }

// AJI_FEATURE
enum class Feature : std::uint32_t {
    dummy     = 0x0001,  // no hardware: placeholder for an unreachable or connecting server
    dynamic   = 0x0002,  // auto-detected; can't be removed
    special   = 0x0004,
    led       = 0x0100,
    button    = 0x0200,
    insocket  = 0x0400,
    jtag      = 0x0800,
    secondary = 0x4000,
    identify  = 0x8000,
};
using Features = Flags<Feature>;

// AJI_DEVFEAT
enum class DeviceFeature : std::uint32_t {
    bad_ir_capture         = 1,
    user01_break_ir        = 2,
    possible_hub           = 4,
    command_response_chain = 8,
};
using DeviceFeatures = Flags<DeviceFeature>;

// AJI_DR_FLAGS
enum class DrFlag : std::uint32_t {
    unused_0       = 1,
    unused_0_omit  = 3,
    unused_x       = 15,
    no_short       = 16,
    end_pause_dr   = 32,
    start_pause_dr = 64,
    no_restore     = 128,
};
using DrFlags = Flags<DrFlag>;

// AJI_RTI_FLAGS
enum class RtiFlag : std::uint32_t {
    accurate_clock = 1,
    exit_rti_state = 2,
};
using RtiFlags = Flags<RtiFlag>;

// AJI_CHAIN_TYPE
enum class ChainType : std::uint32_t { unknown = 0, jtag = 1 };

// AJI_PACK_STYLE. See docs/cpp-wrapper/FINDINGS.md §3.1 for what each defers.
enum class Pack : std::uint32_t {
    never     = 0,   // every call runs before it returns
    automatic = 1,   // write-only calls are queued; calls that read run at once
    manual    = 2,   // everything is queued until flush() or the end of the session
    stream    = 3,
};

// ---------------------------------------------------------------------------
// Hardware (a JTAG chain behind a cable)

// Copied out of AJI_HARDWARE by aji::scan / find*. Owns its strings: the
// library's own copies can be freed or reallocated under you
// (FINDINGS.md §2.2, §2.4).
struct HardwareInfo {
    std::uint32_t              persistent_id = 0;  // the only key that survives reboots
    std::string                hw_name;            // empty for dummy entries
    std::optional<std::string> port;
    std::optional<std::string> device_name;        // user-given name
    std::optional<std::string> server;             // nullopt for the local server
    ChainType                  chain_type = ChainType::unknown;
    Features                   features;
    std::string                server_version;     // empty if unknown (find_by_*)

    bool is_dummy() const { return features.has(Feature::dummy); }
    std::string display_name() const;              // "USB-Blaster [1-1.2] on host"
};

// What add_hardware needs.
struct HardwareSpec {
    std::string                hw_name;            // a type name the server knows
    std::optional<std::string> port;
    std::optional<std::string> device_name;
};

// ---------------------------------------------------------------------------
// Devices on a chain

// One TAP, from read_device_chain. `position` is the index the library uses
// (aji.h:861-862 says 0 is nearest TDI; jtag_client_open.h:233 says 0 is
// nearest TDO; unverified, DESIGN.md O7).
struct TapInfo {
    std::uint32_t              position = 0;
    std::uint32_t              idcode = 0;
    std::uint32_t              idcode_mask = 0;      // 1 bits are "don't care"
    std::uint8_t               ir_length = 0;        // 0 = unknown
    DeviceFeatures             features;
    std::optional<std::string> name;

    bool ir_length_known() const { return ir_length != 0; }
};

// User device database entry (aji_define_device and friends).
struct DeviceSpec {
    std::uint32_t  idcode = 0;
    std::uint32_t  idcode_mask = 0;
    std::uint8_t   ir_length = 0;
    DeviceFeatures features;
    std::string    name;
};

// ---------------------------------------------------------------------------
// SLD (virtual JTAG) nodes and hubs

inline constexpr std::size_t max_hub_depth = 8;     // AJI_MAX_HIERARCHICAL_HUB_DEPTH

// Mirrors AJI_HIER_ID.
struct HierId {
    std::uint32_t                            idcode = 0;
    std::uint8_t                             position_n = 0;   // hierarchy level, 0 = top
    std::array<std::uint8_t, max_hub_depth>  positions{};
    bool operator==(const HierId&) const = default;
};

// Mirrors AJI_HUB_INFO.
struct HubInfo {
    std::array<std::uint32_t, max_hub_depth> bridge_idcode{};  // [0] is meaningless
    std::array<std::uint32_t, max_hub_depth> hub_idcode{};
    bool operator==(const HubInfo&) const = default;
};

// A hub is never listed on its own: it is "the level-L ancestor of node X".
struct SldHubAddr {
    HierId        node;
    std::uint32_t level = 0;
};

// One node from ChainLock::nodes().
struct SldNodeInfo {
    HierId  id;
    HubInfo hubs;   // the AJI_HUB_INFO at the same index as `id`

    std::uint32_t idcode() const { return id.idcode; }
    std::uint32_t depth() const { return id.position_n; }
    SldHubAddr    hub(std::uint32_t level) const { return {id, level}; }
};

// ---------------------------------------------------------------------------
// Claims

// AJI_CLAIM_TYPE
enum class ClaimType : std::uint32_t {
    ir                 = 0x0000,
    ir_shared          = 0x0100,
    ir_shared_overlay  = 0x0300,
    ir_overlaid        = 0x0400,
    ir_shared_overlaid = 0x0500,
    ir_weak            = 0x0800,
    overlay            = 0x0001,
    overlay_shared     = 0x0101,
    overlay_weak       = 0x0801,
};

struct Claim {
    ClaimType     type;
    std::uint64_t value;
};

// The registers an open may use. Order matters: an overlay IR must come
// before the IRs it overlays (aji.h:407-408).
//
//   auto claims = aji::ClaimSet{}
//       .add(aji::ClaimType::ir_shared_overlay,  USER1)
//       .add(aji::ClaimType::ir_shared_overlaid, USER0)
//       .add(aji::ClaimType::overlay_shared,     HUB_IDENT);
class ClaimSet {
public:
    ClaimSet& add(ClaimType t, std::uint64_t value) & { claims_.push_back({t, value}); return *this; }
    ClaimSet&& add(ClaimType t, std::uint64_t value) && { claims_.push_back({t, value}); return std::move(*this); }
    std::span<const Claim> claims() const { return claims_; }
private:
    std::vector<Claim> claims_;
};

// ---------------------------------------------------------------------------
// Bit vectors. The first bit shifted is the least significant bit of byte 0
// (aji.h:1343-1347).

// A borrowed bit vector to write.
struct BitsView {
    std::span<const std::uint8_t> bytes;
    std::uint32_t                 length = 0;    // in bits
};

// An owned bit vector that was read. For a batched scan, `count` items of
// `length` bits each, every item starting on a byte boundary (aji.h:1451-1457).
struct Bits {
    std::vector<std::uint8_t> bytes;
    std::uint32_t             length = 0;    // bits per item
    std::uint32_t             count = 1;

    std::size_t bytes_per_item() const { return (length + 7) / 8; }
    std::span<const std::uint8_t> item(std::uint32_t i) const
    {
        return std::span(bytes).subspan(i * bytes_per_item(), bytes_per_item());
    }
    bool bit(std::uint32_t n, std::uint32_t item_index = 0) const
    {
        return (item(item_index)[n / 8] >> (n % 8)) & 1u;
    }
};

// One data-register scan (aji_access_dr). Bits outside the written window
// are recirculated; read_length == 0 means "don't read".
struct DrScan {
    std::uint32_t           length = 0;          // DR length (whole chain for EntireChain)
    DrFlags                 flags;
    std::uint32_t           write_offset = 0;
    std::optional<BitsView> write;               // write->length bits at write_offset
    std::uint32_t           read_offset = 0;
    std::uint32_t           read_length = 0;
    std::uint32_t           batch = 1;           // repeat count; write->bytes holds batch items
};

} // namespace aji
