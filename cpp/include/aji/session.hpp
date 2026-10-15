// Session<K>: an open that currently holds the connection's lock.
//
// You never construct one. ChainLock::with and Open<K>::locked build it and
// pass it to your callback as a prvalue; when the callback's parameter is
// destroyed (before `with`/`locked` continues), the session flushes queued
// scans and hands the lock back. Take it as `auto s`:
//
//     chain_lock.with(hub, aji::Pack::manual, [](auto s) {
//         s.overlay(HUB_IDENT);
//         return s.dr({.length = 4, .read_length = 4, .batch = 32});
//     });
//
// `auto& s` doesn't compile (a prvalue can't bind to it), and `const auto s`
// allows no scans (they are non-const). See docs/cpp-wrapper/DESIGN.md §5.
//
// Errors: every scan returns a Pending. After the first error in a session,
// queued results are unreliable (the library reports one error per batch and
// fills no buffers after it), so all of them fail, and `with`/`locked`
// returns that first error instead of the callback's value.
#pragma once

#include "aji/detail/session_core.hpp"
#include "aji/error.hpp"
#include "aji/pending.hpp"
#include "aji/types.hpp"

#include <chrono>
#include <type_traits>
#include <utility>

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

namespace detail { struct SessionFactory; }

template<Kind K>
class Session {
public:
    Session(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(const Session&) = delete;
    Session& operator=(Session&&) = delete;
    ~Session() = default;   // the core flushes and hands the lock back

    // Load an instruction into this TAP's IR; the value captured from IR.
    Pending<std::uint32_t> ir(std::uint32_t instruction) requires (K == Kind::tap)
    {
        return Pending(core_.ir_value(instruction));
    }

    // Shift `write.length` bits through IR (for EntireChain: the whole chain's
    // IR, unvalidated); the bits captured.
    Pending<Bits> ir(BitsView write) requires (!is_virtual<K>)
    {
        return Pending(core_.ir_bits(write));
    }

    // SLD nodes and hubs: write the virtual IR (USER1 + DR + USER0); the
    // value captured from the overlay register.
    Pending<std::uint32_t> overlay(std::uint32_t value) requires (is_virtual<K>)
    {
        return Pending(core_.overlay(value));
    }

    // One data-register scan; the bits read (empty if read_length == 0).
    Pending<Bits> dr(const DrScan& scan) { return Pending(core_.dr(scan)); }

    // Run-Test/Idle for `clocks` cycles.
    Pending<void> idle(std::uint32_t clocks, RtiFlags flags = {}) { return Pending(core_.idle(clocks, flags)); }

    // Test-Logic-Reset. Resets every TAP on the chain, whatever K is.
    Pending<void> reset() { return Pending(core_.reset()); }

    // Pause the clock for at least `us`.
    Pending<void> delay(std::chrono::microseconds us) { return Pending(core_.delay(us)); }

    // Run everything queued so far; afterwards their Pendings are ready.
    expected<void> flush() { return core_.flush(); }

    Pack pack() const noexcept { return core_.pack(); }

private:
    friend struct detail::SessionFactory;

    Session(detail::OpenState& open, Pack pack, detail::SessionCore::Release release,
            ChainLock* chain, detail::Handback& out) noexcept
        : core_(open, pack, release, chain, out)
    {}

    detail::SessionCore core_;
};

namespace detail {

// The type of `f(T{...})` where the argument is a prvalue. std::invoke_result
// can't express that: it passes an xvalue, which can't initialise a by-value
// parameter of a non-movable type, so it reports "not invocable" for
// `[](auto s)`. `prvalue<T>()` is never defined; it only appears in decltype.
template<class T>
T prvalue() noexcept;

template<class F, class Arg>
using call_result_t = decltype(std::declval<F&>()(prvalue<Arg>()));

// What `with`/`locked` return for a callback returning R:
//   void -> expected<void>,  expected<T> -> expected<T>,  Pending<T> -> expected<T>,  T -> expected<T>
template<class R> struct settled                 { using type = R; };
template<class T> struct settled<expected<T>>    { using type = T; };
template<class T> struct settled<Pending<T>>     { using type = T; };
template<class R> using settled_t = typename settled<std::remove_cvref_t<R>>::type;

template<class R>
expected<settled_t<R>> settle(R&& r)
{
    using D = std::remove_cvref_t<R>;
    if constexpr (std::is_same_v<D, expected<settled_t<R>>>)
        return std::forward<R>(r);
    else if constexpr (std::is_same_v<D, Pending<settled_t<R>>>)
        return r.get();
    else
        return expected<settled_t<R>>(std::forward<R>(r));
}

// Calls `f` with a prvalue Session and collects the outcome. Whether the
// parameter is destroyed when `f` returns or at the end of the full
// expression below, it is before the next statement ([expr.call]), so the
// handback has happened by the time we read `out`.
struct SessionFactory {
    template<Kind K, class F>
    static auto run(OpenState& open, Pack pack, SessionCore::Release release, ChainLock* chain, F& f)
        -> expected<settled_t<call_result_t<F, Session<K>>>>
    {
        using R = call_result_t<F, Session<K>>;
        Handback out;
        if constexpr (std::is_void_v<R>) {
            f(Session<K>(open, pack, release, chain, out));
            if (out.error)
                return unexpected(*out.error);
            return {};
        } else {
            R result = f(Session<K>(open, pack, release, chain, out));
            if (out.error)
                return unexpected(*out.error);
            return settle(std::move(result));
        }
    }
};

} // namespace detail
} // namespace aji
