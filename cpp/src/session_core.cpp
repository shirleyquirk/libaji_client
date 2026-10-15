// Session bookkeeping: which results are known, and handing the lock back.
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
//     (jtag_client_open.cpp:2151-2157). So the first error fails every
//     outstanding result and is what the session reports.
#include "c_api.hpp"   // first: aji.h must precede every other header

#include "aji/chain_lock.hpp"
#include "aji/detail/session_core.hpp"


namespace aji::detail {

namespace {

template<class T>
SlotPtr<T> make_slot()
{
    return std::make_shared<Slot<T>>();
}

std::uint32_t bytes_for(std::uint32_t bits) { return (bits + 7) / 8; }

} // namespace

// ---------------------------------------------------------------------------
// Opens

void close_open(std::unique_ptr<OpenState> state) noexcept
{
    if (!state)
        return;
    if (state->in_session) {
        // The session still holds the lock on this open. Closing now would
        // leak the connection mutex; the session closes it after the handback.
        state->close_when_session_ends = true;
        state.release();
        return;
    }
    aji_close_device(state->id);   // closes even when it reports an error
}

expected<void> lock_open(OpenState& state, std::chrono::milliseconds timeout, Pack pack)
{
    return check(aji_lock(state.id, static_cast<DWORD>(timeout.count()), static_cast<AJI_PACK_STYLE>(pack)),
                 "aji_lock");
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

// ---------------------------------------------------------------------------
// SessionCore

SessionCore::SessionCore(OpenState& open, Pack pack, Release release, ChainLock* chain, Handback& out) noexcept
    : open_(open), pack_(pack), release_(release), chain_(chain), out_(out)
{
    open_.in_session = true;
}

SessionCore::~SessionCore() { finish(); }

void SessionCore::record(const Error& e)
{
    if (!first_error_)
        first_error_ = e;
}

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

void SessionCore::fail_here(const std::shared_ptr<SlotBase>& slot, Error e)
{
    slot->fail(e);
    record(e);
}

void SessionCore::issued(const std::shared_ptr<SlotBase>& slot, int aji_error, const char* call, bool reads)
{
    if (aji_error != AJI_NO_ERROR) {
        Error e = from_aji(static_cast<AJI_ERROR>(aji_error), call);
        slot->fail(e);
        fail_outstanding(e);
        record(e);
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

SlotPtr<std::uint32_t> SessionCore::ir_value(std::uint32_t instruction)
{
    auto slot = make_slot<std::uint32_t>();
    AJI_ERROR e = aji_access_ir(open_.id, instruction, &slot->value, 0);
    issued(slot, e, "aji_access_ir", true);
    return slot;
}

SlotPtr<Bits> SessionCore::ir_bits(BitsView write)
{
    auto slot = make_slot<Bits>();
    if (write.bytes.size() < bytes_for(write.length)) {
        fail_here(slot, Error{Errc::buffer_too_small, "Session::ir", {}});
        return slot;
    }
    const BYTE* data = write.bytes.data();
    if (pack_ == Pack::stream)
        data = stream_writes_.emplace_back(write.bytes.begin(), write.bytes.end()).data();
    slot->value.length = write.length;
    slot->value.bytes.resize(bytes_for(write.length));
    AJI_ERROR e = aji_access_ir(open_.id, write.length, data, slot->value.bytes.data(), 0);
    issued(slot, e, "aji_access_ir", true);
    return slot;
}

SlotPtr<std::uint32_t> SessionCore::overlay(std::uint32_t value)
{
    auto slot = make_slot<std::uint32_t>();
    AJI_ERROR e = aji_access_overlay(open_.id, value, &slot->value);
    issued(slot, e, "aji_access_overlay", true);
    return slot;
}

SlotPtr<Bits> SessionCore::dr(const DrScan& scan)
{
    auto slot = make_slot<Bits>();
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

    AJI_ERROR e = aji_access_dr(open_.id, scan.length, scan.flags.bits(), scan.write_offset, write_length,
                                write_data, scan.read_offset, scan.read_length, read_data, batch);
    issued(slot, e, "aji_access_dr", read_data != nullptr);
    return slot;
}

SlotPtr<void> SessionCore::idle(std::uint32_t clocks, RtiFlags flags)
{
    auto slot = make_slot<void>();
    issued(slot, aji_run_test_idle(open_.id, clocks, flags.bits()), "aji_run_test_idle", false);
    return slot;
}

SlotPtr<void> SessionCore::reset()
{
    auto slot = make_slot<void>();
    issued(slot, aji_test_logic_reset(open_.id), "aji_test_logic_reset", false);
    return slot;
}

SlotPtr<void> SessionCore::delay(std::chrono::microseconds us)
{
    auto slot = make_slot<void>();
    issued(slot, aji_delay(open_.id, static_cast<DWORD>(us.count())), "aji_delay", false);
    return slot;
}

expected<void> SessionCore::flush()
{
    AJI_ERROR e = aji_flush(open_.id);
    if (e != AJI_NO_ERROR) {
        Error err = from_aji(e, "aji_flush");
        fail_outstanding(err);
        record(err);
        return unexpected(err);
    }
    succeed_outstanding();
    return {};
}

// Hand the lock back to the chain. aji_unlock_lock_chain has three outcomes
// (FINDINGS.md §3.3):
//   ok                     -> the chain holds the lock
//   early deferred error   -> the open still holds it (jtag_client_open.cpp:587-588);
//                             the error is now cleared, so a retry works
//   error after sending    -> nobody holds it (:603-612)
// A retry tells the last two apart: it returns not_locked if the open no
// longer holds the lock.
void SessionCore::hand_back_to_chain() noexcept
{
    AJI_CHAIN* chain_id = chain_->id_;
    AJI_ERROR e = aji_unlock_lock_chain(open_.id, chain_id);
    if (e == AJI_NO_ERROR)
        return;
    record(from_aji(e, "aji_unlock_lock_chain"));

    AJI_ERROR retry = aji_unlock_lock_chain(open_.id, chain_id);
    if (retry == AJI_NO_ERROR)
        return;
    if (retry != AJI_NOT_LOCKED)
        aji_unlock(open_.id);   // state unclear: make sure the open isn't locked before it can be closed

    AJI_ERROR relock = aji_lock_chain(chain_id, static_cast<DWORD>(chain_->relock_timeout_.count()));
    if (relock != AJI_NO_ERROR && relock != AJI_LOCKED) {   // AJI_LOCKED: we hold it after all
        record(from_aji(relock, "aji_lock_chain (re-lock after failed handback)"));
        chain_->lost_ = true;
    }
}

void SessionCore::finish() noexcept
{
    // 1. Run whatever is still queued, so results and errors are known.
    if (!outstanding_.empty() || pack_ != Pack::never) {
        AJI_ERROR e = aji_flush(open_.id);
        if (e != AJI_NO_ERROR) {
            Error err = from_aji(e, "aji_flush (end of session)");
            fail_outstanding(err);
            record(err);
        } else {
            succeed_outstanding();
        }
    }

    // 2. Give the lock back.
    if (release_ == Release::to_chain) {
        hand_back_to_chain();
    } else if (AJI_ERROR e = aji_unlock(open_.id); e != AJI_NO_ERROR) {
        record(from_aji(e, "aji_unlock"));   // aji_unlock releases the link even on error
    }

    // 3. Anything still outstanding never ran.
    if (!outstanding_.empty())
        fail_outstanding(first_error_.value_or(Error{Errc::failure, "end of session", {}}));

    open_.in_session = false;
    if (open_.close_when_session_ends) {
        // The Open was destroyed during the session (close_open); it's ours now.
        aji_close_device(open_.id);
        delete &open_;
    }

    if (first_error_)
        out_.error = std::move(first_error_);
}

} // namespace aji::detail
