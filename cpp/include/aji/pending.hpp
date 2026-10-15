// Pending<T>: the result of a scan that may not have run yet.
//
// With Pack::manual (and write-only calls under Pack::automatic) the library
// queues the scan and returns at once; the result, or the error, arrives at
// the next flush or when the session ends (FINDINGS.md §3). The read buffer
// lives in the Pending's slot, which the library writes into, so it is valid
// for as long as the library may use it.
#pragma once

#include "aji/detail/slot.hpp"
#include "aji/error.hpp"

#include <memory>
#include <type_traits>

namespace aji {

template<class T>
class [[nodiscard]] Pending {
public:
    // Internal: Pendings are created by Session.
    explicit Pending(detail::SlotPtr<T> slot) : slot_(std::move(slot)) {}

    // True once the result (or its error) is known.
    bool ready() const { return slot_->state != detail::SlotBase::State::queued; }

    // The value; Errc::not_flushed before the session has flushed this
    // scan's batch; the batch's error if the flush failed.
    expected<T> get() const
    {
        using State = detail::SlotBase::State;
        switch (slot_->state) {
        case State::queued:
            return unexpected(Error{Errc::not_flushed, "Pending::get", {}});
        case State::failed:
            return unexpected(slot_->error);
        case State::done:
            break;
        }
        if constexpr (std::is_void_v<T>)
            return {};
        else
            return slot_->value;
    }

private:
    detail::SlotPtr<T> slot_;
};

} // namespace aji
