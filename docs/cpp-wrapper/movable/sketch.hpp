// Sketch: the same wrapper with movable lock objects and no callbacks.
// Declarations only, to compare call sites (read_idcode.cpp,
// virtual_jtag.cpp here vs cpp/examples/). Reuses the value types,
// errors and Pending<T> from the real wrapper.
#pragma once

#include "aji/error.hpp"
#include "aji/pending.hpp"
#include "aji/session.hpp"   // Kind
#include "aji/types.hpp"

#include <chrono>
#include <concepts>
#include <vector>

namespace aji2 {

using aji::Errc;
using aji::Error;
using aji::expected;
using aji::Kind;
using aji::Pack;
using aji::Pending;
using aji::unexpected;
using namespace std::chrono_literals;

class ChainLock;
template<Kind K> class Open;
template<Kind K> class Session;

// Unchanged from the real wrapper, except that locking returns an object.
class Chain {
public:
    const aji::HardwareInfo& info() const;
    expected<ChainLock> lock(std::chrono::milliseconds timeout) const;   // aji_lock_chain
};

template<std::predicate<const aji::HardwareInfo&> P>
expected<Chain> find(P&& pred, std::chrono::milliseconds timeout = 2s);

// The chain lock, held while this object is non-empty. Move-only.
//
// Empty after: being moved from, unlock(), a successful hand_to(), or losing
// the lock. Every method on an empty lock returns Errc::not_locked; called
// from a thread other than the one that locked, Errc::wrong_thread
// (the library's mutex belongs to that thread). Destroying a held lock
// unlocks it; destroying it on the wrong thread calls std::terminate, as
// ~std::thread does for a joinable thread.
class ChainLock {
public:
    ChainLock(ChainLock&&) noexcept;
    ChainLock& operator=(ChainLock&&) noexcept;
    ~ChainLock();

    explicit operator bool() const noexcept;   // holds the lock?
    const aji::HardwareInfo& info() const;

    expected<void> unlock();                    // aji_unlock_chain, with its error

    expected<std::vector<aji::TapInfo>>     taps(bool auto_scan = true);
    expected<std::vector<aji::SldNodeInfo>> nodes(std::uint32_t tap_position);
    expected<std::uint32_t>                 parameter(const std::string& name);

    expected<Open<Kind::tap>>      open_tap(std::uint32_t tap_position, const aji::ClaimSet&, const std::string& app);
    expected<Open<Kind::sld_node>> open_node(std::uint32_t tap_position, const aji::SldNodeInfo&,
                                             const aji::ClaimSet&, const std::string& app);

    // Atomic handoff (aji_unlock_chain_lock). On success this lock is empty
    // and the session holds the lock. On failure, this lock still holds it
    // if the library kept it, and is empty if nobody holds it any more:
    // check bool(*this).
    template<Kind K>
    expected<Session<K>> hand_to(Open<K>& open, Pack pack);
};

// An open, while it holds the lock. Move-only; same emptiness and thread
// rules as ChainLock. Destroying a held session flushes and unlocks it
// (the lock goes to nobody, not back to the chain).
template<Kind K>
class Session {
public:
    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;
    ~Session();

    explicit operator bool() const noexcept;

    // Scans: unchanged from the real wrapper.
    Pending<std::uint32_t> ir(std::uint32_t instruction) requires (K == Kind::tap);
    Pending<aji::Bits>     ir(aji::BitsView write) requires (!aji::is_virtual<K>);
    Pending<std::uint32_t> overlay(std::uint32_t value) requires (aji::is_virtual<K>);
    Pending<aji::Bits>     dr(const aji::DrScan& scan);
    expected<void>         flush();

    // Atomic handback (aji_unlock_lock_chain), for a session that came from
    // ChainLock::hand_to. Flushes first; scan errors show up in the
    // Pendings, not here. Fails with Errc::lock_lost only if the chain
    // could not be re-locked either. This session is empty afterwards.
    expected<ChainLock> hand_back();

    // Atomic move to another open on the same chain (aji_unlock_lock).
    template<Kind K2>
    expected<Session<K2>> hand_to(Open<K2>& other, Pack pack);

    expected<void> unlock();                    // aji_unlock: the lock goes to nobody
};

template<Kind K>
class Open {
public:
    Open(Open&&) noexcept;
    Open& operator=(Open&&) noexcept;
    ~Open();   // aji_close_device; deferred if a session on it is still held

    expected<Session<K>> lock(std::chrono::milliseconds timeout, Pack pack);   // aji_lock
};

} // namespace aji2
