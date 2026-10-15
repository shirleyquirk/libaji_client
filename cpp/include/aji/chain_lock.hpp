// ChainLock: a chain whose lock this thread holds (aji_lock_chain).
//
// You never construct one: Chain::locked passes it to your callback as a
// prvalue and unlocks when the callback's parameter is destroyed. Take it as
// `auto cl` (or `auto&&`); helper functions can take `aji::ChainLock&`.
//
// While held, no other client can use the chain, and no other thread of this
// process can talk to the same server (FINDINGS.md §1.1). Keep it short.
#pragma once

#include "aji/detail/handles.hpp"
#include "aji/error.hpp"
#include "aji/open.hpp"
#include "aji/session.hpp"
#include "aji/types.hpp"

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aji {

class Chain;

namespace detail {
struct ChainLockResult {
    std::optional<Error> error;
};
}

class ChainLock {
public:
    ChainLock(const ChainLock&) = delete;
    ChainLock(ChainLock&&) = delete;
    ChainLock& operator=(const ChainLock&) = delete;
    ChainLock& operator=(ChainLock&&) = delete;
    ~ChainLock();                                   // aji_unlock_chain, unless the lock was lost

    const HardwareInfo& info() const noexcept { return info_; }

    // ---- chain operations ----

    // The TAPs on the chain (aji_read_device_chain), scanning if needed.
    expected<std::vector<TapInfo>> taps(bool auto_scan = true);

    // The SLD nodes behind the TAP at `tap_position` (aji_get_nodes).
    // Empty if the TAP has no SLD hub.
    expected<std::vector<SldNodeInfo>> nodes(std::uint32_t tap_position);

    expected<std::uint32_t>             parameter(const std::string& name);
    expected<void>                      set_parameter(const std::string& name, std::uint32_t value);
    expected<std::vector<std::uint8_t>> parameter_block(const std::string& name,
                                                        std::span<const std::uint8_t> send = {});

    // ---- opening ----

    expected<Open<Kind::tap>>          open_tap(std::uint32_t tap_position, const ClaimSet& claims,
                                                const std::string& application);
    expected<Open<Kind::sld_node>>     open_node(std::uint32_t tap_position, const SldNodeInfo& node,
                                                 const ClaimSet& claims, const std::string& application);
    expected<Open<Kind::sld_hub>>      open_hub(std::uint32_t tap_position, const SldHubAddr& hub,
                                                const ClaimSet& claims, const std::string& application);
    expected<Open<Kind::entire_chain>> open_entire(ChainType style, const std::string& application);

    // ---- handing the lock to an open ----

    // Hands this chain's lock to `open` (aji_unlock_chain_lock), runs `f` with
    // a prvalue Session<K>, then hands it back (aji_unlock_lock_chain). If the
    // handback fails, the chain is re-locked with this lock's original
    // timeout; if that fails too, this ChainLock is lost and every later call
    // returns Errc::lock_lost (DESIGN.md §6).
    //
    // While `f` runs, calls on this ChainLock return Errc::lock_handed_away.
    template<Kind K, class F>
    auto with(Open<K>& open, Pack pack, F&& f)
        -> expected<detail::settled_t<detail::call_result_t<F, Session<K>>>>
    {
        if (!open.state_)
            return unexpected(Error{Errc::invalid_open_id, "ChainLock::with", {}});
        if (auto e = hand_to(*open.state_, pack); !e)
            return unexpected(e.error());
        struct Reset {
            ChainLock& lock;
            ~Reset() { lock.handed_away_ = false; }
        } reset{*this};
        return detail::SessionFactory::run<K>(*open.state_, pack, detail::SessionCore::Release::to_chain,
                                              this, f);
    }

private:
    friend class Chain;
    friend class detail::SessionCore;

    ChainLock(AJI_CHAIN* id, std::chrono::milliseconds timeout, const HardwareInfo& info,
              detail::ChainLockResult& out) noexcept
        : id_(id), relock_timeout_(timeout), info_(info), out_(out)
    {}

    expected<void> usable(const char* call) const;
    expected<void> hand_to(detail::OpenState& open, Pack pack);
    static std::unique_ptr<detail::OpenState> adopt(AJI_OPEN* id);

    AJI_CHAIN*                 id_;
    std::chrono::milliseconds  relock_timeout_;
    const HardwareInfo&        info_;
    detail::ChainLockResult&   out_;
    bool                       handed_away_ = false;
    bool                       lost_ = false;
};

} // namespace aji
