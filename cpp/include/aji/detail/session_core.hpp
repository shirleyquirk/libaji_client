// Non-template machinery behind Session<K>, Open<K> and ChainLock::with.
// Defined in src/session_core.cpp.
#pragma once

#include "aji/detail/handles.hpp"
#include "aji/detail/pin.hpp"
#include "aji/detail/slot.hpp"
#include "aji/error.hpp"
#include "aji/pending.hpp"
#include "aji/types.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace aji {
class ChainLock;
}

namespace aji::detail {

// Heap-allocated state of one open (AJI_OPEN_ID). Open<K> owns it; a Session
// keeps a raw pointer, which stays valid if the Open is moved. If the Open is
// destroyed while a session is running, ownership passes to the session,
// which closes the open after handing the lock back: closing a locked open
// would leak the connection mutex (FINDINGS.md §1.2).
struct OpenState {
    AJI_OPEN* id = nullptr;
    Pin       pin;
    bool      in_session = false;
    bool      close_when_session_ends = false;
    std::unique_ptr<std::function<void(std::uint32_t)>> progress;
};

// Destroys an open: aji_close_device, or defers to the running session.
void close_open(std::unique_ptr<OpenState> state) noexcept;

// aji_lock on an open (Open<K>::locked).
expected<void> lock_open(OpenState& state, std::chrono::milliseconds timeout, Pack pack);

void set_progress(OpenState& state, std::function<void(std::uint32_t)> fn);

// Where a session's outcome goes, since ~Session can't return it.
struct Handback {
    std::optional<Error> error;
};

// One locked session on one open.
class SessionCore {
public:
    enum class Release {
        to_chain,   // hand the lock back to the ChainLock (aji_unlock_lock_chain)
        unlock,     // plain aji_unlock (session from Open<K>::locked)
    };

    SessionCore(OpenState& open, Pack pack, Release release, ChainLock* chain, Handback& out) noexcept;
    ~SessionCore();     // flush, hand back, report into `out`

    SessionCore(const SessionCore&) = delete;
    SessionCore& operator=(const SessionCore&) = delete;

    SlotPtr<std::uint32_t> ir_value(std::uint32_t instruction);
    SlotPtr<Bits>          ir_bits(BitsView write);
    SlotPtr<std::uint32_t> overlay(std::uint32_t value);
    SlotPtr<Bits>          dr(const DrScan& scan);
    SlotPtr<void>          idle(std::uint32_t clocks, RtiFlags flags);
    SlotPtr<void>          reset();
    SlotPtr<void>          delay(std::chrono::microseconds us);
    expected<void>         flush();

    Pack pack() const noexcept { return pack_; }

private:
    // Book-keeping after each library call (rules in session_core.cpp).
    void issued(const std::shared_ptr<SlotBase>& slot, int aji_error, const char* call, bool reads);
    void fail_here(const std::shared_ptr<SlotBase>& slot, Error e);
    void fail_outstanding(const Error& e);
    void succeed_outstanding() noexcept;
    void record(const Error& e);
    void finish() noexcept;
    void hand_back_to_chain() noexcept;

    OpenState&                             open_;
    Pack                                   pack_;
    Release                                release_;
    ChainLock*                             chain_;
    Handback&                              out_;
    std::vector<std::shared_ptr<SlotBase>> outstanding_;
    std::vector<std::vector<std::uint8_t>> stream_writes_;   // Pack::stream doesn't copy big writes
    std::optional<Error>                   first_error_;
};

} // namespace aji::detail
