// Storage for one scan result, shared between a Session (which the library
// writes into) and the Pending<T> handed to the user.
#pragma once

#include "aji/error.hpp"
#include "aji/types.hpp"

#include <memory>

namespace aji::detail {

struct SlotBase {
    enum class State { queued, done, failed };

    State state = State::queued;
    Error error;

    virtual ~SlotBase() = default;

    // Only a queued slot changes state: a result that already arrived is
    // not undone by a later failure in the same session.
    void succeed() noexcept { if (state == State::queued) state = State::done; }
    void fail(const Error& e) { if (state == State::queued) { state = State::failed; error = e; } }
};

template<class T>
struct Slot : SlotBase { T value{}; };

template<>
struct Slot<void> : SlotBase {};

template<class T>
using SlotPtr = std::shared_ptr<Slot<T>>;

} // namespace aji::detail
