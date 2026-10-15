// Chain: one JTAG chain (cable) found by aji::scan or aji::find*.
//
// Copyable. Holds the library's AJI_CHAIN_ID, a HardwareInfo by value, and a
// pin: while any Chain (or ChainLock, Open) is alive, management functions
// return Errc::chains_in_use instead of letting the library free the chain
// object (FINDINGS.md §2.4, management.hpp).
#pragma once

#include "aji/chain_lock.hpp"
#include "aji/detail/handles.hpp"
#include "aji/detail/pin.hpp"
#include "aji/error.hpp"
#include "aji/types.hpp"

#include <chrono>
#include <vector>

namespace aji {

namespace detail { struct ChainAccess; }

class Chain {
public:
    Chain(const Chain&) = default;
    Chain& operator=(const Chain&) = default;
    Chain(Chain&& other) noexcept;              // `other` becomes empty and stops pinning
    Chain& operator=(Chain&& other) noexcept;
    ~Chain() = default;

    const HardwareInfo& info() const noexcept { return info_; }
    explicit operator bool() const noexcept { return id_ != nullptr; }   // false once moved from

    // The TAPs the library already knows (aji_read_device_chain without a
    // lock). Errc::chain_not_configured if the chain hasn't been scanned yet;
    // ChainLock::taps() scans.
    expected<std::vector<TapInfo>> cached_taps() const;

    // aji_lock_chain. Waits up to `timeout` for other clients; the same
    // timeout is used to re-lock if a handoff back to the chain fails.
    [[nodiscard]] expected<ChainLock> lock(std::chrono::milliseconds timeout) const;

private:
    friend struct detail::ChainAccess;
    Chain(AJI_CHAIN* id, HardwareInfo info) : id_(id), info_(std::move(info)) {}

    AJI_CHAIN*   id_;
    HardwareInfo info_;
    detail::Pin  pin_;
};

} // namespace aji
