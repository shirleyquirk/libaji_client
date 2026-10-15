// Open<K>: an opened TAP, whole chain, SLD node or SLD hub (an AJI_OPEN_ID).
//
// Created by ChainLock::open_*; closed (aji_close_device) when destroyed.
// Move-only. May outlive the ChainLock scope it was opened in: closing needs
// no chain lock.
//
// To scan, it needs the lock: either hand it the chain's lock with
// ChainLock::with (the usual way), or take it directly with locked().
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

    // Take the lock with aji_lock (no ChainLock involved) and run `f` with a
    // Session<K>; aji_unlock afterwards. Fails with Errc::server_active after
    // `timeout` if this thread already holds a lock on the same server
    // (FINDINGS.md §1.1).
    template<class F>
    auto locked(std::chrono::milliseconds timeout, Pack pack, F&& f)
        -> expected<detail::settled_t<detail::call_result_t<F, Session<K>>>>
    {
        if (!state_)
            return unexpected(Error{Errc::invalid_open_id, "Open::locked", {}});
        if (auto e = detail::lock_open(*state_, timeout, pack); !e)
            return unexpected(e.error());
        return detail::SessionFactory::run<K>(*state_, pack, detail::SessionCore::Release::unlock, nullptr, f);
    }

    // aji_register_progress_callback: `fn(bits)` with the number of user data
    // bits written since the last lock. May be called from another thread.
    // An empty function unregisters.
    void on_progress(std::function<void(std::uint32_t)> fn)
    {
        if (state_)
            detail::set_progress(*state_, std::move(fn));
    }

    explicit operator bool() const noexcept { return state_ != nullptr; }   // false once moved from

private:
    friend class ChainLock;
    explicit Open(std::unique_ptr<detail::OpenState> state) : state_(std::move(state)) {}

    std::unique_ptr<detail::OpenState> state_;
};

} // namespace aji
