// Session<K>: an open while it holds the lock. Move-only.
//
// You get one from ChainLock::hand_to (the chain's lock, handed over
// atomically) or from Open<K>::lock (aji_lock). A Session is either *held*
// or *empty*; it becomes empty when moved from, after unlock(), hand_back()
// or hand_to(), or when a failed handoff loses the lock.
//
//     auto s = cl.hand_to(tap, aji::Pack::manual);     // cl is now empty
//     (void)s->ir(IDCODE);
//     auto id = s->dr({.length = 32, .read_length = 32});
//     s->flush();                                      // id.get() is ready
//     auto back = s->hand_back();                      // the chain's lock again
//
// Every call on an empty session fails with Errc::empty_lock, and on a
// thread other than the one that took the lock with Errc::wrong_thread (the
// library's mutex belongs to that thread, FINDINGS.md §1.1). Destroying a
// held session flushes it and unlocks (the lock goes to nobody, not back to
// a chain); destroying it on the wrong thread calls std::terminate, as
// ~std::thread does for a joinable thread.
//
// Errors. Scans return Pending<T>: with Pack::manual they only run at the
// next flush. The first error fails every result still outstanding, because
// the library reports one error per batch and fills no buffers after it
// (FINDINGS.md §3.2). flush() returns that error; unlock(), hand_back() and
// hand_to() flush first but report only what happened to the lock, so call
// flush() if you need to know whether write-only scans worked.
#pragma once

#include "aji/detail/session_core.hpp"
#include "aji/error.hpp"
#include "aji/pending.hpp"
#include "aji/types.hpp"

#include <chrono>
#include <memory>

namespace aji {

// What an AJI_OPEN_ID was opened as. Decides which scans compile.
enum class Kind {
    tap,            // aji_open_device: one TAP controller
    entire_chain,   // aji_open_entire_device_chain: the whole scan chain, exclusively
    sld_node,       // aji_open_node: a virtual JTAG (SLD) node
    sld_hub,        // aji_open_hub: an SLD hub
};

template<Kind K>
inline constexpr bool is_virtual = (K == Kind::sld_node || K == Kind::sld_hub);

class ChainLock;
template<Kind K> class Open;

template<Kind K>
class Session {
public:
    Session() = default;                                    // empty
    Session(Session&&) noexcept = default;
    Session& operator=(Session&&) noexcept = default;       // releases what *this held first
    ~Session() = default;                                   // the core flushes and unlocks if held

    explicit operator bool() const noexcept { return core_ && core_->held(); }

    // ---- scans ----

    // Load an instruction into this TAP's IR; the value captured from IR.
    Pending<std::uint32_t> ir(std::uint32_t instruction) requires (K == Kind::tap)
    {
        return scan<std::uint32_t>("Session::ir", [&] { return core_->ir_value(instruction); });
    }

    // Shift `write.length` bits through IR (for entire_chain: the whole
    // chain's IR, unvalidated); the bits captured.
    Pending<Bits> ir(BitsView write) requires (!is_virtual<K>)
    {
        return scan<Bits>("Session::ir", [&] { return core_->ir_bits(write); });
    }

    // SLD nodes and hubs: write the virtual IR (USER1 + DR + USER0); the
    // value captured from the overlay register.
    Pending<std::uint32_t> overlay(std::uint32_t value) requires (is_virtual<K>)
    {
        return scan<std::uint32_t>("Session::overlay", [&] { return core_->overlay(value); });
    }

    // One data-register scan; the bits read (empty if read_length == 0).
    Pending<Bits> dr(const DrScan& scan_) { return scan<Bits>("Session::dr", [&] { return core_->dr(scan_); }); }

    // Run-Test/Idle for `clocks` cycles.
    Pending<void> idle(std::uint32_t clocks, RtiFlags flags = {})
    {
        return scan<void>("Session::idle", [&] { return core_->idle(clocks, flags); });
    }

    // Test-Logic-Reset. Resets every TAP on the chain, whatever K is.
    Pending<void> reset() { return scan<void>("Session::reset", [&] { return core_->reset(); }); }

    // Pause the clock for at least `us`.
    Pending<void> delay(std::chrono::microseconds us)
    {
        return scan<void>("Session::delay", [&] { return core_->delay(us); });
    }

    // Run everything queued; afterwards every Pending issued so far is ready.
    expected<void> flush()
    {
        if (!core_)
            return unexpected(Error{Errc::empty_lock, "Session::flush", {}});
        return core_->flush();
    }

    Pack pack() const noexcept { return core_ ? core_->pack() : Pack::never; }

    // ---- releasing the lock (each leaves this session empty) ----

    // aji_unlock: the lock goes to nobody.
    [[nodiscard]] expected<void> unlock()
    {
        if (!core_)
            return unexpected(Error{Errc::empty_lock, "Session::unlock", {}});
        auto r = core_->unlock();
        if (!core_->held())   // still held only if refused (wrong thread)
            core_.reset();
        return r;
    }

    // Atomic handback to the chain (aji_unlock_lock_chain), for a session
    // that came from ChainLock::hand_to; Errc::not_from_chain otherwise. If
    // the library fails the handback, the chain is re-locked (no longer
    // atomically); Errc::lock_lost if even that fails. Defined in
    // chain_lock.hpp.
    [[nodiscard]] expected<ChainLock> hand_back();

    // Atomic move to another open on the same chain (aji_unlock_lock); the
    // pack style carries over. On success this session is empty. On failure
    // it still holds the lock, unless the lock was lost (Errc::lock_lost,
    // and this session is empty). Defined in open.hpp.
    template<Kind K2>
    [[nodiscard]] expected<Session<K2>> hand_to(Open<K2>& next);

private:
    friend class ChainLock;
    template<Kind> friend class Open;
    template<Kind> friend class Session;

    explicit Session(std::unique_ptr<detail::SessionCore> core) : core_(std::move(core)) {}

    template<class T, class Fn>
    Pending<T> scan(const char* call, Fn&& fn)
    {
        if (!core_) {
            auto slot = std::make_shared<detail::Slot<T>>();
            slot->fail(Error{Errc::empty_lock, call, {}});
            return Pending<T>(std::move(slot));
        }
        return Pending<T>(fn());
    }

    std::unique_ptr<detail::SessionCore> core_;
};

} // namespace aji
