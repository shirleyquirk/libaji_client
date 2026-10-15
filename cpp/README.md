# aji C++ wrapper

C++23 RAII wrapper around `libaji_client` (`src/h/aji.h`). Design and the
library behaviour behind it: [`docs/cpp-wrapper/DESIGN.md`](../docs/cpp-wrapper/DESIGN.md),
[`docs/cpp-wrapper/FINDINGS.md`](../docs/cpp-wrapper/FINDINGS.md).

```cpp
#include "aji/aji.hpp"
using namespace std::chrono_literals;

aji::expected<aji::Bits> read_idcode(const aji::Chain& chain)
{
    auto cl = chain.lock(10s);                                   // aji_lock_chain
    if (!cl) return aji::unexpected(cl.error());
    auto tap = cl->open_tap(0, aji::ClaimSet{}.add(aji::ClaimType::ir_shared, 6), "me");
    if (!tap) return aji::unexpected(tap.error());
    auto s = cl->hand_to(*tap, aji::Pack::manual);               // atomic handoff; *cl is empty now
    if (!s) return aji::unexpected(s.error());
    (void)s->ir(6);                                              // IDCODE
    auto idcode = s->dr({.length = 32, .read_length = 32});      // Pending<Bits>: queued
    if (auto f = s->flush(); !f) return aji::unexpected(f.error());
    return idcode.get();
}   // ~s unlocks, ~tap closes, ~cl is already empty
```

## Requirements

- gcc 14 or clang 19 (C++23 `std::expected`; clang 18 with libstdc++ doesn't provide it).
- CMake 3.20+.
- `libaji_client.so` built from this repo (below).

## Build

The C library first, from the repo root. Ubuntu's gcc enables
`-fcf-protection` by default, which conflicts with the library's
`-mindirect-branch` flags, so turn it off:

```sh
git submodule update --init
./bootstrap
./configure CFLAGS=-fcf-protection=none CXXFLAGS=-fcf-protection=none
make -j
```

Then the wrapper:

```sh
cmake -S cpp -B cpp/build -DCMAKE_CXX_COMPILER=g++-14
cmake --build cpp/build
ctest --test-dir cpp/build
```

With sanitizers (needs the C library built with them too; `tests/ub/run.sh`
does that in-tree):

```sh
tests/ub/run.sh
cmake -S cpp -B cpp/build -DCMAKE_CXX_COMPILER=g++-14 -DAJI_SANITIZE=ON
cmake --build cpp/build && ctest --test-dir cpp/build
```

`AJI_CLIENT_LIBRARY` points CMake at a different `libaji_client.so`.

## Tests

`ctest` runs two kinds, neither needing hardware nor jtagd:

- **`no_hardware.*`**: the wrapper against the real library. Servers at
  `localhost:1`/`:2`, where nothing listens, give real dummy chains. Covers
  scan, find, the management guard (including the two use-after-free
  scenarios from `tests/ub/05` and `06`, now refused), pin counting, and
  lock failure.
- **`must_not_compile.*`**: 14 misuses that must be compile errors (scans on
  the wrong `Kind`, copying a lock or an `Open`, discarding a `Pending` or the
  result of `lock`/`hand_to`/`hand_back`, constructing a held lock...). Each must fail
  with a specific diagnostic, not just fail.

Everything that needs a lock (opens, sessions, handoffs, deferred results)
can only run against real hardware.

## Hardware test plan

Built in `cpp/build/examples/`.

1. **`aji_list`**, then `jtagconfig`. Both should show the same chains, TAP
   IDCODEs and names, SLD nodes and top-level design hash.
   Exercises: scan, lock, taps, nodes, open_hub, `hand_to`/`hand_back` + `Pack::manual`,
   overlay, batched dr, `Pending` after `flush()`.
2. **`aji_idcode <cable-substring> [instr=6]`**. Self-checking: reads each
   TAP's IDCODE via `hand_to`/`hand_back` in `Pack::never`, `automatic` and
   `manual`, then via `Open::lock` after the chain lock is released. Expect `PASS` on every line for
   Intel FPGAs (IDCODE = 6); other TAPs need their IDCODE instruction.
   Also checks: the `ChainLock` is empty while handed off and held after
   `hand_back`, open-to-open `Session::hand_to`, `wrong_thread` from another
   thread. Exercises all three pack styles, every handoff, opens outliving the
   chain lock, `aji_lock`/`aji_unlock`.
3. **Contention**: run `aji_idcode` in a loop while the Quartus Programmer or
   SignalTap uses the same cable. Expected: `chain_in_use`/`timeout` errors,
   never a hang or a crash; afterwards both tools work.
4. **Unplug** the cable in the middle of a loop of `aji_idcode`. Expected:
   errors (`bad_hardware`, `invalid_chain_id`, ...), no crash. Once all
   handles are gone, `aji_list` works again.
5. **Facts for DESIGN.md O7**: the order of `TapInfo::position` (is 0 nearest
   TDI or TDO?) on a multi-TAP chain, and whether `Session<sld_node>` should
   allow raw IR access.

Useful to report back for any failure: the full `Error::message()` output
(it includes the failing `aji_*` call and `aji_get_error_info()`), the jtagd
version, and the cable type.

## Behaviour summary

- **Errors**: every fallible call returns `aji::expected<T>`; `Error` has an
  `Errc` (mirrors `AJI_ERROR`, plus wrapper codes from 1000), the failing call,
  and the library's error text.
- **Management functions** (`scan`, `find*`, `add_hardware`, `remove`,
  `add_remote_server`, `enable_remote_clients`, the device database) return
  `Errc::chains_in_use` while any `Chain` or `Open` is alive, because they can
  make the library free those objects (FINDINGS.md §2.4). Drop handles before
  rescanning.
- **Locks** are move-only objects: `Chain::lock` gives a `ChainLock`;
  `ChainLock::hand_to(open)` moves the lock atomically into a `Session<K>`,
  `Session::hand_back()` moves it back, `Session::hand_to(other)` moves it to
  another open on the chain; `Open::lock` takes it directly. An object that
  doesn't hold the lock (moved from, released, handed off) returns
  `Errc::empty_lock`; a call from a thread other than the one that locked
  returns `Errc::wrong_thread`, and destroying a held lock on another thread
  calls `std::terminate`.
- **Sessions**: every scan returns `Pending<T>`. Results are ready after
  `flush()`; the first error fails every result still outstanding, and
  `flush()` returns it. Releasing (`unlock`/`hand_back`/`hand_to`/destructor)
  flushes first but reports only what happened to the lock.
- **Handoff failures**: the old holder keeps the lock, or the wrapper re-takes
  it; if that fails too, `Errc::lock_lost` and both objects are empty
  (DESIGN.md §6).
- **Library bugs left in place** (no patches): dotted IPv4 server addresses
  overflow a buffer inside the library; removing a temporary server is a
  `new[]`/`delete` mismatch; with `QUARTUS_JTAG_CLIENT_NO_LOCAL_SERVER` set and
  no remote servers configured, `scan()` crashes inside the library
  (FINDINGS.md §4).
