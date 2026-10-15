# Movable locks instead of callbacks?

**Status: adopted.** `cpp/` now uses movable `ChainLock`/`Session<K>` with
atomic `hand_to`/`hand_back`. This page is the reasoning behind that
decision; [`../DESIGN.md`](../DESIGN.md) §5-6 describe the result, and
[`cpp/examples`](../../../cpp/examples) show it in use.

The question was: since every method on a lock still needs a runtime check,
why not make the locks movable and drop the callbacks? And do the atomic lock
handoffs matter, or can we swap non-atomically?

Answer: **yes, make them movable, and keep the atomic handoffs.** They
don't conflict: an atomic handoff is the lock moving from one object into
another, the way a `std::unique_lock` moves.

## 1. The checks are there either way

| Runtime check on every method | Callbacks (before) | Movable (now) |
|---|---|---|
| "My lock is with an open right now" | `handed_away_`: the lambda can capture the `ChainLock` by reference and call it inside `with` | gone: `hand_to` empties the `ChainLock`, so this is the "empty" check below |
| "I lost the lock" (a handback and the re-lock both failed) | `lost_` | the "empty" check |
| "Moved from / released" | — | the "empty" check |
| "Wrong thread" | — (can't happen: the object can't leave the callback) | needed: the library's mutex belongs to the thread that locked it |

So callbacks save one check (the thread), not all of them. The core
invariant is the same in both: *an object that isn't empty holds the lock.*
C++ has no linear types, so neither design gets it fully at compile time.
`clang-tidy`'s `bugprone-use-after-move` catches the commonest movable
mistake (using `cl` after `cl.hand_to(...)` or `std::move(cl)`) statically.

## 2. What the callbacks buy, and what movable costs

| Callbacks guarantee at compile time | With movable locks |
|---|---|
| A lock can't move to another thread | Runtime: `Errc::wrong_thread` on every call; destruction on the wrong thread calls `std::terminate` (the `~std::thread` precedent). This matters: the library checks lock ownership by thread (`JTAG_MUTEX::is_claimed`, `jtag_platform.h:239`), so `close_device` on another thread waits 20 s and fails (`jtag_client_open.cpp:640-645`). |
| A lock can't outlive its scope (can't be stored, returned, kept for minutes) | Nothing stops it. Holding a lock blocks every other client on that chain, and every other thread of this process on that server. That's a documentation matter, as with any mutex. |
| An `Open` can't be destroyed while its session runs | Same runtime mechanism as now: the close is deferred until the session releases (`OpenState::close_when_session_ends`). Already implemented. |

And what the callbacks cost, all of which goes away:

- **Plumbing.** `SessionFactory`, `settle`, `settled_t`, `call_result_t`
  (needed because `std::invoke_result` can't express a prvalue argument),
  `Handback` and `ChainLockResult` exist only so the destructor of an object
  the user never names can report errors.
- **Errors at release.** A destructor can't return one, so today the
  handback's outcome is written to a side channel and merged into the
  callback's result. With movable locks, `unlock()`/`hand_back()` return
  `expected` like everything else; the destructor is only the fallback.
- **Control flow.** Early returns from inside nested lambdas, return types
  that have to agree, `auto s` vs `auto& s` rules: all gone. Ordinary
  functions can take, return and hold locks (`virtual_jtag.cpp`'s
  `transfer(ChainLock&, ...)`).
- **The "drop a handle" problem** (`usb = aji::unexpected(...)` in the first
  draft of `list_hardware.cpp`) is unrelated to locks and stays the same
  either way: scoping blocks or `std::optional`.

## 3. Do the atomic handoffs matter?

`aji_unlock_chain_lock` / `aji_unlock_lock_chain` / `aji_unlock_lock` move
the lock in one server message. The non-atomic version is two calls: unlock,
then lock.

What atomicity buys:

1. **No other client gets in between.** Between your chain-level work
   (reading the TAPs and SLD nodes, opening) and your first scan on the open,
   nobody else can use the chain. Non-atomically, the Quartus Programmer could
   reconfigure the FPGA in that gap, so the node list you just read describes
   a design that's gone. *How the server treats opens on a reconfigured
   device I haven't checked; this is the reason the library's own hub
   discovery does it atomically (`jtag_client_hub.cpp:204-205`, "Pass lock
   from chain to device atomically").*
2. **You can't lose the lock halfway.** Non-atomically, the second `lock`
   can fail with `chain_in_use`/`timeout` because someone else took it in
   the gap. Then the error has to be handled in the middle of a sequence.
3. **One round trip instead of two.** It matters for remote servers.
4. Not a correctness requirement for the server: for servers older than
   protocol 7 the library itself falls back to unlock + lock "and hope"
   (`jtag_client_open.cpp:516-522`, `:578-584`).

What it costs in the movable design: nothing extra. `hand_to`/`hand_back`
are the same size as `unlock` + `lock`, and the failure cases are the ones
already handled today (DESIGN.md §6). The non-atomic route stays available
for anyone who wants it: `cl.unlock(); auto s = open.lock(...)`.

So keep both. They aren't in tension once the lock is a movable object:
`cl.hand_to(open)` empties `cl` and returns a `Session` holding the lock;
`s.hand_back()` empties `s` and returns a `ChainLock`.

One honest wrinkle: on failure, `hand_to` can't always say "the lock is
still in `cl`". After a failure *after sending* nobody holds it
(`jtag_client_open.cpp:544`), and the wrapper's re-lock may fail too. So the
rule is "on failure, check `bool(cl)`" (DESIGN.md §6). The callback design has
the same three outcomes, hidden behind `lost_`.

## 4. Side by side

The callback versions of the examples are in git history (commit `646cc93`,
`cpp/examples/`). Compared with the movable versions now in `cpp/examples/`:
`read_idcode` becomes a plain function that reads top to bottom, returns
early with ordinary `return`, and releases in destruction order (`~Session`
unlocks, `~Open` closes, `~ChainLock` is already empty). `virtual_jtag` and
`aji_list` gain helpers that borrow the caller's `ChainLock&`, hand the lock
to a node or hub and back (`cl = std::move(*back)`), which the callback
design couldn't express.

## 5. What changed in `cpp/`

- Removed: `SessionFactory`, `settle`/`settled_t`/`call_result_t`,
  `Handback`, `ChainLockResult`, `Chain::locked`, `Open::locked`,
  `ChainLock::with`, and the must-not-compile cases that test callback rules
  (4 of 12: `session_by_lvalue_ref`, `session_escapes`,
  `chain_lock_by_lvalue_ref`, `chain_lock_escapes`).
- Added: move operations with an empty state; owner thread id + check;
  `Chain::lock`, `Open::lock`, `ChainLock::unlock/hand_to`,
  `Session::hand_back/hand_to/unlock`; `Errc::empty_lock`, `wrong_thread`,
  `not_from_chain`; a runtime test for empty objects (no hardware needed) and
  checks for the thread rule and every handoff in `aji_idcode` (hardware).
- Unchanged: values, errors, `Pending`, `SessionCore`'s scan bookkeeping, the
  management guard, `OpenState` deferral, the examples' logic.

## 6. Recommendation, ranked

1. **Movable locks with atomic `hand_to`/`hand_back`** (chosen). Same
   runtime checks as now plus one thread check, much less machinery, and
   ordinary control flow; atomicity kept at no cost.
2. Movable locks with only non-atomic swaps (`unlock` then `lock`). Slightly
   smaller API; loses (1)-(3) above. I'd only choose this if the handoff
   failure rules turn out confusing in practice.
3. Keep the callbacks. The strongest compile-time scoping, but it costs the
   plumbing in §2 and saves only the thread check. I no longer recommend it.
