// Open<K>: an opened TAP, whole chain, SLD node or SLD hub (an AJI_OPEN_ID).
//
// Created by ChainLock::open_*; closed (aji_close_device) when destroyed.
// Move-only. May outlive the ChainLock it was opened with: closing needs no
// chain lock. If it is destroyed while a Session holds its lock, the close
// waits until that session releases the lock.
//
// To scan it needs the lock, as a Session<K>: either handed over from the
// chain (ChainLock::hand_to, atomic) or taken directly with lock().
#pragma once

#include "aji/detail/session_core.hpp"
#include "aji/session.hpp"

#include <chrono>
#include <functional>
#include <memory>

namespace aji {

template<Kind K>
class Open {
public:
    Open() = default;                                   // empty
    Open(Open&&) noexcept = default;
    Open& operator=(Open&& other) noexcept
    {
        if (this != &other) {
            detail::close_open(std::move(state_));
            state_ = std::move(other.state_);
        }
        return *this;
    }
    Open(const Open&) = delete;
    Open& operator=(const Open&) = delete;
    ~Open() { detail::close_open(std::move(state_)); }

    explicit operator bool() const noexcept { return state_ != nullptr; }   // false once moved from

    // Take the lock with aji_lock (no ChainLock involved). Fails with
    // Errc::server_active after `timeout` if this thread already holds a
    // lock on the same server (FINDINGS.md §1.1), Errc::locked if a session
    // already holds this open.
    [[nodiscard]] expected<Session<K>> lock(std::chrono::milliseconds timeout, Pack pack)
    {
        auto core = detail_lock(timeout, pack);
        if (!core)
            return unexpected(core.error());
        return Session<K>(std::move(*core));
    }

    // aji_register_progress_callback: `fn(bits)` with the number of user data
    // bits written since the last lock. May be called from another thread.
    // An empty function unregisters.
    void on_progress(std::function<void(std::uint32_t)> fn)
    {
        if (state_)
            detail::set_progress(*state_, std::move(fn));
    }

private:
    friend class ChainLock;
    template<Kind> friend class Session;

    explicit Open(std::unique_ptr<detail::OpenState> state) : state_(std::move(state)) {}

    expected<std::unique_ptr<detail::SessionCore>> detail_lock(std::chrono::milliseconds timeout, Pack pack);

    std::unique_ptr<detail::OpenState> state_;
};

namespace detail {
expected<std::unique_ptr<SessionCore>> lock_open(OpenState* state, std::chrono::milliseconds timeout, Pack pack);
expected<std::unique_ptr<SessionCore>> hand_open_to_open(SessionCore& from, OpenState* next);
}

template<Kind K>
expected<std::unique_ptr<detail::SessionCore>> Open<K>::detail_lock(std::chrono::milliseconds timeout, Pack pack)
{
    return detail::lock_open(state_.get(), timeout, pack);
}

template<Kind K>
template<Kind K2>
expected<Session<K2>> Session<K>::hand_to(Open<K2>& next)
{
    if (!core_)
        return unexpected(Error{Errc::empty_lock, "Session::hand_to", {}});
    auto core = detail::hand_open_to_open(*core_, next.state_.get());
    if (!core_->held())   // handed over, or lost
        core_.reset();
    if (!core)
        return unexpected(core.error());
    return Session<K2>(std::move(*core));
}

} // namespace aji
