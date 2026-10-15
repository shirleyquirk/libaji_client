// Session bookkeeping: which results are known, and releasing or handing on
// the lock.
//
// Library behaviour this relies on (docs/cpp-wrapper/FINDINGS.md §3):
//   - Pack::never: every call runs before it returns.
//   - Pack::automatic: calls that read run at once; write-only calls are
//     queued (jtag_client_open.h:348-350, jtag_client_open.cpp:749-750).
//   - Pack::manual / stream: calls are queued.
//   - A call that runs at once first drains everything queued before it
//     (send_receive waits for every outstanding packet), so its success means
//     the earlier ones succeeded too.
//   - An error from any call may belong to an earlier queued scan, and after
//     an error the library fills no more read buffers
//     (jtag_client_open.cpp:2151-2157). So an error fails every outstanding
//     result.
#include "c_api.hpp"   // first: aji.h must precede every other header

#include "aji/detail/session_core.hpp"

#include <exception>

namespace aji::detail {

namespace {

template<class T>
SlotPtr<T> make_slot()
{
    return std::make_shared<Slot<T>>();
}

std::uint32_t bytes_for(std::uint32_t bits) { return (bits + 7) / 8; }

DWORD ms(std::chrono::milliseconds t) { return static_cast<DWORD>(t.count()); }

} // namespace

expected<void> check_lock(bool held, std::thread::id owner, const char* call)
{
    if (!held)
        return unexpected(Error{Errc::empty_lock, call, {}});
    if (owner != std::this_thread::get_id())
        return unexpected(Error{Errc::wrong_thread, call, {}});
    return {};
}

// ---------------------------------------------------------------------------
// Opens

void close_open(std::unique_ptr<OpenState> state) noexcept
{
    if (!state)
        return;
    if (state->in_session) {
        // A session still holds the lock on this open. Closing now would leak
        // the connection mutex; the session closes it after releasing.
        state->close_when_session_ends = true;
        state.release();
        return;
    }
    aji_close_device(state->id);   // closes even when it reports an error
}

void set_progress(OpenState& state, std::function<void(std::uint32_t)> fn)
{
    if (!fn) {
        aji_register_progress_callback(state.id, nullptr, nullptr);
        state.progress.reset();
        return;
    }
    auto holder = std::make_unique<std::function<void(std::uint32_t)>>(std::move(fn));
    aji_register_progress_callback(
        state.id,
        [](void* handle, DWORD bits) { (*static_cast<std::function<void(std::uint32_t)>*>(handle))(bits); },
        holder.get());
    state.progress = std::move(holder);
}

expected<std::unique_ptr<SessionCore>> lock_open(OpenState* state, std::chrono::milliseconds timeout, Pack pack)
{
    if (state == nullptr)
        return unexpected(Error{Errc::invalid_open_id, "Open::lock", {}});
    if (state->in_session)
        return unexpected(Error{Errc::locked, "Open::lock", {}});
    if (auto e = check(aji_lock(state->id, ms(timeout), static_cast<AJI_PACK_STYLE>(pack)), "aji_lock"); !e)
        return unexpected(e.error());
    return std::make_unique<SessionCore>(*state, pack, std::nullopt, timeout);
}

expected<std::unique_ptr<SessionCore>> hand_open_to_open(SessionCore& from, OpenState* next)
{
    if (auto u = from.usable("Session::hand_to"); !u)
        return unexpected(u.error());
    if (next == nullptr)
        return unexpected(Error{Errc::invalid_open_id, "Session::hand_to", {}});
    if (next->in_session)
        return unexpected(Error{Errc::locked, "Session::hand_to", {}});
    if (auto e = from.hand_to(*next); !e)
        return unexpected(e.error());
    // aji_unlock_lock carries the pack style over (jtag_client_open.cpp:482).
    return std::make_unique<SessionCore>(*next, from.pack(), from.chain(), from.relock_timeout());
}

// ---------------------------------------------------------------------------
// SessionCore

SessionCore::SessionCore(OpenState& open, Pack pack, std::optional<ChainSeed> chain,
                         std::chrono::milliseconds relock_timeout) noexcept
    : open_(&open), pack_(pack), chain_(std::move(chain)), relock_timeout_(relock_timeout),
      owner_(std::this_thread::get_id())
{
    open_->in_session = true;
}

SessionCore::~SessionCore()
{
    if (!held_)
        return;
    if (owner_ != std::this_thread::get_id())
        std::terminate();   // the library's mutex belongs to the owner thread; see session.hpp
    flush_quietly();
    aji_unlock(open_->id);  // releases the connection mutex even on error
    held_ = false;
    released();
}

expected<void> SessionCore::usable(const char* call) const { return check_lock(held_, owner_, call); }

void SessionCore::fail_outstanding(const Error& e)
{
    for (auto& slot : outstanding_)
        slot->fail(e);
    outstanding_.clear();
}

void SessionCore::succeed_outstanding() noexcept
{
    for (auto& slot : outstanding_)
        slot->succeed();
    outstanding_.clear();
}

void SessionCore::fail_here(const std::shared_ptr<SlotBase>& slot, Error e) { slot->fail(e); }

template<class T>
SlotPtr<T> SessionCore::start(const char* call)
{
    auto slot = make_slot<T>();
    if (auto u = usable(call); !u)
        slot->fail(u.error());
    return slot;
}

void SessionCore::issued(const std::shared_ptr<SlotBase>& slot, int aji_error, const char* call, bool reads)
{
    if (aji_error != AJI_NO_ERROR) {
        Error e = from_aji(static_cast<AJI_ERROR>(aji_error), call);
        slot->fail(e);
        fail_outstanding(e);
        return;
    }
    const bool ran_now = pack_ == Pack::never || (pack_ == Pack::automatic && reads);
    if (ran_now) {
        slot->succeed();
        succeed_outstanding();
    } else {
        outstanding_.push_back(slot);
    }
}

#define AJI_START(T, call)                         \
    auto slot = start<T>(call);                    \
    if (slot->state == SlotBase::State::failed)    \
        return slot

SlotPtr<std::uint32_t> SessionCore::ir_value(std::uint32_t instruction)
{
    AJI_START(std::uint32_t, "Session::ir");
    AJI_ERROR e = aji_access_ir(open_->id, instruction, &slot->value, 0);
    issued(slot, e, "aji_access_ir", true);
    return slot;
}

SlotPtr<Bits> SessionCore::ir_bits(BitsView write)
{
    AJI_START(Bits, "Session::ir");
    if (write.bytes.size() < bytes_for(write.length)) {
        fail_here(slot, Error{Errc::buffer_too_small, "Session::ir", {}});
        return slot;
    }
    const BYTE* data = write.bytes.data();
    if (pack_ == Pack::stream)
        data = stream_writes_.emplace_back(write.bytes.begin(), write.bytes.end()).data();
    slot->value.length = write.length;
    slot->value.bytes.resize(bytes_for(write.length));
    AJI_ERROR e = aji_access_ir(open_->id, write.length, data, slot->value.bytes.data(), 0);
    issued(slot, e, "aji_access_ir", true);
    return slot;
}

SlotPtr<std::uint32_t> SessionCore::overlay(std::uint32_t value)
{
    AJI_START(std::uint32_t, "Session::overlay");
    AJI_ERROR e = aji_access_overlay(open_->id, value, &slot->value);
    issued(slot, e, "aji_access_overlay", true);
    return slot;
}

SlotPtr<Bits> SessionCore::dr(const DrScan& scan)
{
    AJI_START(Bits, "Session::dr");
    const std::uint32_t batch = scan.batch == 0 ? 1 : scan.batch;

    const BYTE*   write_data = nullptr;
    std::uint32_t write_length = 0;
    if (scan.write) {
        write_length = scan.write->length;
        if (scan.write->bytes.size() < std::size_t(bytes_for(write_length)) * batch) {
            fail_here(slot, Error{Errc::buffer_too_small, "Session::dr", {}});
            return slot;
        }
        write_data = scan.write->bytes.data();
        if (pack_ == Pack::stream)
            write_data = stream_writes_.emplace_back(scan.write->bytes.begin(), scan.write->bytes.end()).data();
    }

    BYTE* read_data = nullptr;
    if (scan.read_length > 0) {
        slot->value.length = scan.read_length;
        slot->value.count = batch;
        slot->value.bytes.resize(std::size_t(bytes_for(scan.read_length)) * batch);
        read_data = slot->value.bytes.data();
    }

    AJI_ERROR e = aji_access_dr(open_->id, scan.length, scan.flags.bits(), scan.write_offset, write_length,
                                write_data, scan.read_offset, scan.read_length, read_data, batch);
    issued(slot, e, "aji_access_dr", read_data != nullptr);
    return slot;
}

SlotPtr<void> SessionCore::idle(std::uint32_t clocks, RtiFlags flags)
{
    AJI_START(void, "Session::idle");
    issued(slot, aji_run_test_idle(open_->id, clocks, flags.bits()), "aji_run_test_idle", false);
    return slot;
}

SlotPtr<void> SessionCore::reset()
{
    AJI_START(void, "Session::reset");
    issued(slot, aji_test_logic_reset(open_->id), "aji_test_logic_reset", false);
    return slot;
}

SlotPtr<void> SessionCore::delay(std::chrono::microseconds us)
{
    AJI_START(void, "Session::delay");
    issued(slot, aji_delay(open_->id, static_cast<DWORD>(us.count())), "aji_delay", false);
    return slot;
}

#undef AJI_START

expected<void> SessionCore::flush_quietly() noexcept
{
    if (outstanding_.empty() && pack_ == Pack::never)
        return {};
    AJI_ERROR e = aji_flush(open_->id);
    if (e != AJI_NO_ERROR) {
        Error err = from_aji(e, "aji_flush");
        fail_outstanding(err);
        return unexpected(err);
    }
    succeed_outstanding();
    return {};
}

expected<void> SessionCore::flush()
{
    if (auto u = usable("Session::flush"); !u)
        return u;
    return flush_quietly();
}

void SessionCore::released() noexcept
{
    stream_writes_.clear();
    if (!outstanding_.empty())   // only if the final flush failed; already failed, but be sure
        fail_outstanding(Error{Errc::failure, "session released", {}});
    open_->in_session = false;
    if (open_->close_when_session_ends) {
        // The Open was destroyed while we held its lock (close_open); it's ours.
        aji_close_device(open_->id);
        delete open_;
        open_ = nullptr;
    }
}

expected<void> SessionCore::unlock()
{
    if (auto u = usable("Session::unlock"); !u)
        return u;
    flush_quietly();                 // errors land in the Pendings; see session.hpp
    AJI_ERROR e = aji_unlock(open_->id);   // releases the connection mutex even on error
    held_ = false;
    released();
    return check(e, "aji_unlock");
}

// Hand the lock back to the chain. aji_unlock_lock_chain has three outcomes
// (FINDINGS.md §3.3):
//   ok                     -> the chain holds the lock
//   early deferred error   -> the open still holds it (jtag_client_open.cpp:587-588);
//                             the error is now cleared, so a retry works
//   error after sending    -> nobody holds it (:603-612)
// A retry tells the last two apart: it returns not_locked if the open no
// longer holds the lock. If nobody holds it, re-lock the chain (no longer
// atomically).
expected<ChainSeed> SessionCore::hand_back()
{
    if (auto u = usable("Session::hand_back"); !u)
        return unexpected(u.error());
    if (!chain_)
        return unexpected(Error{Errc::not_from_chain, "Session::hand_back", {}});
    flush_quietly();

    AJI_CHAIN* chain_id = chain_->id;
    AJI_ERROR e = aji_unlock_lock_chain(open_->id, chain_id);
    if (e != AJI_NO_ERROR) {
        const Error first = from_aji(e, "aji_unlock_lock_chain");
        AJI_ERROR retry = aji_unlock_lock_chain(open_->id, chain_id);
        if (retry != AJI_NO_ERROR) {
            if (retry != AJI_NOT_LOCKED)
                aji_unlock(open_->id);   // state unclear: make sure the open isn't locked before it can be closed
            held_ = false;
            released();
            AJI_ERROR relock = aji_lock_chain(chain_id, ms(chain_->relock_timeout));
            if (relock != AJI_NO_ERROR && relock != AJI_LOCKED)   // AJI_LOCKED: we hold it after all
                return unexpected(Error{Errc::lock_lost, "Session::hand_back", first.message()});
            return *chain_;
        }
    }
    held_ = false;
    released();
    return *chain_;
}

// aji_unlock_lock (open -> open). Early failures leave this open locked; a
// failure after sending leaves nobody holding the lock
// (jtag_client_open.cpp:474-486). aji_lock on this open tells them apart: it
// returns AJI_LOCKED at once if we still hold it (:313-314), and otherwise
// re-takes it.
expected<void> SessionCore::hand_to(OpenState& next)
{
    flush_quietly();
    AJI_ERROR e = aji_unlock_lock(open_->id, next.id);
    if (e == AJI_NO_ERROR) {
        held_ = false;
        released();
        return {};
    }
    const Error first = from_aji(e, "aji_unlock_lock");
    AJI_ERROR relock = aji_lock(open_->id, ms(relock_timeout_), static_cast<AJI_PACK_STYLE>(pack_));
    if (relock == AJI_NO_ERROR || relock == AJI_LOCKED)
        return unexpected(first);   // still ours
    held_ = false;
    released();
    return unexpected(Error{Errc::lock_lost, "Session::hand_to", first.message()});
}

} // namespace aji::detail
