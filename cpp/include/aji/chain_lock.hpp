// ChainLock: a chain's lock (aji_lock_chain). Move-only.
//
// From Chain::lock. Either *held* or *empty*: it becomes empty when moved
// from, after unlock(), after a successful hand_to(), or when a failed
// handoff loses the lock. Every call on an empty lock fails with
// Errc::empty_lock, and on a thread other than the one that locked with
// Errc::wrong_thread (FINDINGS.md §1.1). Destroying a held lock unlocks it;
// destroying it on the wrong thread calls std::terminate.
//
// While held, no other client can use the chain, and no other thread of this
// process can talk to the same server. Keep it short.
//
//     auto cl = chain.lock(10s);
//     auto taps = cl->taps();
//     auto tap = cl->open_tap(0, claims, "me");
//     auto s = cl->hand_to(*tap, aji::Pack::manual);   // atomic; *cl is now empty
//     ...
//     auto back = s->hand_back();                      // atomic; a held ChainLock again
#pragma once

#include "aji/detail/handles.hpp"
#include "aji/detail/pin.hpp"
#include "aji/detail/session_core.hpp"
#include "aji/error.hpp"
#include "aji/open.hpp"
#include "aji/session.hpp"
#include "aji/types.hpp"

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace aji {

class Chain;

class ChainLock {
public:
    ChainLock() = default;                          // empty
    ChainLock(ChainLock&& other) noexcept;
    ChainLock& operator=(ChainLock&& other) noexcept;   // releases what *this held first
    ChainLock(const ChainLock&) = delete;
    ChainLock& operator=(const ChainLock&) = delete;
    ~ChainLock();                                   // aji_unlock_chain if held

    explicit operator bool() const noexcept { return held_; }

    // The chain this lock is (or was) for.
    const HardwareInfo& info() const noexcept { return seed_.info; }

    // aji_unlock_chain, reporting its error. Empty afterwards (unless refused
    // with wrong_thread).
    [[nodiscard]] expected<void> unlock();

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

    // Atomic handoff (aji_unlock_chain_lock). On success this lock is empty
    // and the returned session holds the lock; get it back with
    // Session::hand_back. On failure this lock still holds the lock, unless
    // the library dropped it and re-locking failed too: then the error is
    // Errc::lock_lost and this lock is empty (DESIGN.md §6).
    template<Kind K>
    [[nodiscard]] expected<Session<K>> hand_to(Open<K>& open, Pack pack)
    {
        auto core = hand_to_core(open.state_.get(), pack);
        if (!core)
            return unexpected(core.error());
        return Session<K>(std::move(*core));
    }

private:
    friend class Chain;
    template<Kind> friend class Session;

    explicit ChainLock(detail::ChainSeed seed);     // held, by this thread

    expected<void> usable(const char* call) const;
    expected<std::unique_ptr<detail::SessionCore>> hand_to_core(detail::OpenState* open, Pack pack);
    static std::unique_ptr<detail::OpenState> adopt(AJI_OPEN* id, AJI_CHAIN* chain);
    void release() noexcept;                        // unlock if held; terminate on the wrong thread

    detail::ChainSeed          seed_;
    std::optional<detail::Pin> pin_;                // engaged while held
    std::thread::id            owner_;
    bool                       held_ = false;
};

template<Kind K>
expected<ChainLock> Session<K>::hand_back()
{
    if (!core_)
        return unexpected(Error{Errc::empty_lock, "Session::hand_back", {}});
    if (!core_->chain())
        return unexpected(Error{Errc::not_from_chain, "Session::hand_back", {}});
    auto seed = core_->hand_back();
    if (!core_->held())
        core_.reset();
    if (!seed)
        return unexpected(seed.error());
    return ChainLock(std::move(*seed));
}

} // namespace aji
