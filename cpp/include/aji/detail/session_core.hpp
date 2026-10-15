// Non-template machinery behind Session<K>, Open<K> and ChainLock.
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
#include <thread>
#include <vector>

namespace aji::detail {

// Heap-allocated state of one open (AJI_OPEN_ID). Open<K> owns it; a session
// keeps a raw pointer, which stays valid if the Open is moved. If the Open is
// destroyed while a session holds the lock, ownership passes to the session,
// which closes the open once it has released the lock: closing a locked open
// would leak the connection mutex (FINDINGS.md §1.2).
struct OpenState {
    AJI_OPEN*  id = nullptr;
    AJI_CHAIN* chain = nullptr;        // the chain it was opened on
    Pin        pin;
    bool       in_session = false;
    bool       close_when_session_ends = false;
    std::unique_ptr<std::function<void(std::uint32_t)>> progress;
};

// Destroys an open: aji_close_device, or defers to the session holding it.
void close_open(std::unique_ptr<OpenState> state) noexcept;

void set_progress(OpenState& state, std::function<void(std::uint32_t)> fn);

// What a session needs to give the lock back to its chain.
struct ChainSeed {
    AJI_CHAIN*                id = nullptr;
    HardwareInfo              info;
    std::chrono::milliseconds relock_timeout{};
};

// Common checks for lock objects: holds a lock, on the thread that took it.
expected<void> check_lock(bool held, std::thread::id owner, const char* call);

// One open while it holds the lock. Owned by Session<K> through a
// unique_ptr, so its address (which slots and OpenState rely on) is stable
// across moves of the Session.
class SessionCore {
public:
    // `chain` is set when the lock came from a ChainLock (hand_back possible).
    SessionCore(OpenState& open, Pack pack, std::optional<ChainSeed> chain,
                std::chrono::milliseconds relock_timeout) noexcept;
    ~SessionCore();     // if still held: flush, aji_unlock

    SessionCore(const SessionCore&) = delete;
    SessionCore& operator=(const SessionCore&) = delete;

    expected<void> usable(const char* call) const;

    SlotPtr<std::uint32_t> ir_value(std::uint32_t instruction);
    SlotPtr<Bits>          ir_bits(BitsView write);
    SlotPtr<std::uint32_t> overlay(std::uint32_t value);
    SlotPtr<Bits>          dr(const DrScan& scan);
    SlotPtr<void>          idle(std::uint32_t clocks, RtiFlags flags);
    SlotPtr<void>          reset();
    SlotPtr<void>          delay(std::chrono::microseconds us);
    expected<void>         flush();

    // Releasing the lock. Each leaves this core empty (except where noted).
    expected<void>      unlock();                                    // aji_unlock
    expected<ChainSeed> hand_back();                                 // aji_unlock_lock_chain
    // aji_unlock_lock. On success this core is empty and the caller builds
    // the new session on `next`. On failure this core still holds the lock
    // unless it couldn't be re-taken (then empty).
    expected<void>      hand_to(OpenState& next);

    Pack                            pack() const noexcept { return pack_; }
    const std::optional<ChainSeed>& chain() const noexcept { return chain_; }
    std::chrono::milliseconds       relock_timeout() const noexcept { return relock_timeout_; }
    bool                            held() const noexcept { return held_; }

private:
    template<class T> SlotPtr<T> start(const char* call);   // a slot, failed at once if not usable

    // Book-keeping after each library call (rules in session_core.cpp).
    void issued(const std::shared_ptr<SlotBase>& slot, int aji_error, const char* call, bool reads);
    void fail_here(const std::shared_ptr<SlotBase>& slot, Error e);
    void fail_outstanding(const Error& e);
    void succeed_outstanding() noexcept;
    expected<void> flush_quietly() noexcept;    // flush, settling every outstanding slot
    void released() noexcept;                   // lock gone: settle leftovers, close a deferred open

    OpenState*                             open_;
    Pack                                   pack_;
    std::optional<ChainSeed>               chain_;
    std::chrono::milliseconds              relock_timeout_;
    std::thread::id                        owner_;
    bool                                   held_ = true;
    std::vector<std::shared_ptr<SlotBase>> outstanding_;
    std::vector<std::vector<std::uint8_t>> stream_writes_;   // Pack::stream doesn't copy big writes
};

} // namespace aji::detail
