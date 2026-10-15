# aji C++ wrapper

C++23 RAII wrapper around `libaji_client` (`src/h/aji.h`). Design and the
library behaviour behind it: [`docs/cpp-wrapper/DESIGN.md`](../docs/cpp-wrapper/DESIGN.md),
[`docs/cpp-wrapper/FINDINGS.md`](../docs/cpp-wrapper/FINDINGS.md).

```cpp
#include "aji/aji.hpp"
using namespace std::chrono_literals;

auto chain = aji::find([](const aji::HardwareInfo& h) { return h.hw_name.starts_with("USB-Blaster"); });
if (!chain) return fail(chain.error());

auto idcode = chain->locked(10s, [](auto cl) -> aji::expected<aji::Bits> {
    auto tap = cl.open_tap(0, aji::ClaimSet{}.add(aji::ClaimType::ir_shared, 6), "me");
    if (!tap) return aji::unexpected(tap.error());
    return cl.with(*tap, aji::Pack::manual, [](auto s) {
        (void)s.ir(6);                                   // IDCODE
        return s.dr({.length = 32, .read_length = 32});  // Pending<Bits>, flushed by the time `with` returns
    });
});
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
- **`must_not_compile.*`**: 12 misuses that must be compile errors (scans on
  the wrong `Kind`, `auto&` sessions, sessions or locks escaping their
  callback, copying an `Open`, discarding a `Pending`...). Each must fail
  with a specific diagnostic, not just fail.

Everything that needs a lock (opens, sessions, handoffs, deferred results)
can only run against real hardware.

## Hardware test plan

Built in `cpp/build/examples/`.

1. **`aji_list`**, then `jtagconfig`. Both should show the same chains, TAP
   IDCODEs and names, SLD nodes and top-level design hash.
   Exercises: scan, locked, taps, nodes, open_hub, `with` + `Pack::manual`,
   overlay, batched dr, `Pending` after `flush()`.
2. **`aji_idcode <cable-substring> [instr=6]`**. Self-checking: reads each
   TAP's IDCODE via `with` in `Pack::never`, `automatic` and `manual`, then via
   `Open::locked` after the chain lock is gone. Expect `PASS` on every line for
   Intel FPGAs (IDCODE = 6); other TAPs need their IDCODE instruction.
   Exercises: open_tap, all three pack styles, the handback, opens outliving
   the chain lock, `aji_lock`/`aji_unlock`.
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
- **Locks** exist only inside callbacks: `Chain::locked` passes a
  `ChainLock`, `ChainLock::with`/`Open::locked` pass a `Session<K>`, all as
  prvalues. Take them as `auto cl` / `auto s`.
- **Sessions**: every scan returns `Pending<T>`. Results are ready after
  `flush()` or when the session ends. After the first error in a session,
  queued results fail and `with` returns that error.
- **Handback failures**: the chain is re-locked; if that fails too, the
  `ChainLock` is lost and returns `Errc::lock_lost` (DESIGN.md §6).
- **Library bugs left in place** (no patches): dotted IPv4 server addresses
  overflow a buffer inside the library; removing a temporary server is a
  `new[]`/`delete` mismatch; `scan()` never passes timeout 0
  (FINDINGS.md §4).
