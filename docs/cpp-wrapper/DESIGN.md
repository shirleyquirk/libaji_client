# C++ RAII wrapper for `aji.h`: design plan

Status: **proposal, waiting for sign-off. No wrapper code has been written yet.**
Target: C++23 (deducing `this`, `std::expected`), gcc 14 / clang 18.

The library behaviour this plan relies on, with line references and sanitizer
evidence, is in [`FINDINGS.md`](FINDINGS.md). Section numbers like "F§1.1"
point there.

---

## 1. Decisions in one screen

| # | Decision | Why (finding) |
|---|---|---|
| D1 | A **separate named type for each holder of the lock** (`ChainLock`, `Session<K>`), not `T<LockState>` | There is one lock per server connection and it moves between holders (F§1.1). Per-object lock states would compile `Hardware<Locked>` + `Tap<Locked>` together, which fails at runtime. |
| D2 | Lock holders are **neither copyable nor movable** | The lock is a pthread mutex owned by the thread that took it (F§1.1). |
| D3 | The lock is passed to an open **within a scope**: `chain_lock.with(open, f)` | It mirrors the library's own usage (F§1.1) and makes the handoff back automatic. |
| D4 | **One open type, templated on its kind**: `Open<K>` / `Session<K>`, `K ∈ {Tap, EntireChain, SldNode, SldHub}`, with operations gated by `requires` | The four kinds share most scan operations, with differences (F§1.2, and §2 below). |
| D5 | `AJI_HARDWARE` is **discarded after copying**: `HardwareInfo` holds owned strings, and `Chain` holds the id plus a `HardwareInfo` **by value** | The strings and the id are library-owned and can be freed under us (F§2.1, F§2.2, F§2.4). |
| D6 | **Free functions in `namespace aji`**, no `Registry` object. The functions that can make the library free chains ("management" functions) refuse to run while any `Chain`/`ChainLock`/`Open` is alive. | Those calls free chain objects, strings and server connections, including because *another process* edited `~/.jtag.conf` (F§2.4, tests 01, 05, 06). |
| D7 | **Deferred scans return `Pending<T>`**, which can only be read after a successful flush | Results and errors arrive late; buffers aren't filled on error (F§3). |
| D8 | Handing the lock back **can fail**, and the wrapper handles it (retry, then re-lock or mark the lock lost) | F§3.3. |

---

## 2. Types

### Plain value types (copyable, hold no library resource)

| Type | Contents |
|---|---|
| `HardwareInfo` | `persistent_id`, `hw_name`, `port`, `device_name`, `server` (owned `std::string`s; `std::optional` where the library allows `NULL`), `chain_type`, `features` (flag set), `server_version`. **No `AJI_CHAIN_ID`.** |
| `TapInfo` | `AJI_DEVICE` fields (owned name) + `position` (the index `aji_read_device_chain` returns, which `open_device` takes). The direction is unclear: `aji.h:861-862` says index 0 is nearest TDI, but `jtag_client_open.h:233` says `m_position` 0 is near TDO. Open item O7. |
| `SldNodeInfo` | `AJI_HIER_ID` + the `AJI_HUB_INFO` at the same index (`get_nodes` returns parallel arrays; `jtagconfig.cpp:540-555` sizes them together) |
| `SldHubAddr` | `(SldNodeInfo, level)`. Hubs are never listed directly; a hub is "the level-L ancestor of node X" (`aji_open_hub`, `aji.h:2088`). Obtained with `node.hub(level)`. |
| `ClaimSet` | a list of `AJI_CLAIM2`, with builders: `ClaimSet::tap({ir...})`, `ClaimSet::sld_node()`, `ClaimSet::sld_hub()` |
| `aji::Error` | `enum class` mirroring `AJI_ERROR`, plus wrapper-only errors (`lock_lost`, `stale_chain`), with a `std::error_category` |

### Handles

| Type | Wraps | Ownership | Copy / move |
|---|---|---|---|
| `Chain` | `AJI_CHAIN_ID` + `HardwareInfo` (by value) | doesn't own the chain object; holds a *pin* that blocks management calls (§3) | copyable (each copy is a pin) |
| `ChainLock` | chain holds the lock | releases with `aji_unlock_chain` | neither |
| `Open<K>` | `AJI_OPEN_ID` | owner; destructor calls `aji_close_device` | move-only |
| `Session<K>` | an `Open<K>` holds the lock | gives the lock back on scope exit | neither |

### The four kinds of `Open`

| `K` | Created by (on `ChainLock`) | Operations available on `Session<K>` |
|---|---|---|
| `Tap` | `open_tap(pos, claims)` → `aji_open_device` | `ir(DWORD)`, `ir(bits)`, `dr`, `idle`, `reset`, `delay` |
| `EntireChain` | `open_entire(style)` → `aji_open_entire_device_chain` | `ir(bits)` only (the `DWORD` form is rejected, `aji.h:1295-1297`), `dr` over the whole chain length, `idle`, `reset`, `delay`. Exclusive: no other client can use the chain. |
| `SldNode` | `open_node(pos, node, claims)` → `aji_open_node` (`AJI_HIER_ID` overload) | `overlay`, `dr`, `idle`, `reset`, `delay`. Not `ir`: a node's "IR" is the virtual IR set with `overlay`. Whether the server rejects a raw `access_ir` on a node open still needs checking (O7). |
| `SldHub` | `open_hub(pos, hub_addr, claims)` → `aji_open_hub` | same as `SldNode` |

```cpp
enum class Kind { Tap, EntireChain, SldNode, SldHub };

template<Kind K>
inline constexpr bool is_virtual = (K == Kind::SldNode || K == Kind::SldHub);

template<Kind K>
class Session {
public:
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;   // implicitly no move either

    Pending<Bits>  dr(DrScan const&);
    Pending<DWORD> ir(DWORD instruction)          requires (K == Kind::Tap);
    Pending<Bits>  ir(BitsView write)              requires (!is_virtual<K>);
    Pending<DWORD> overlay(DWORD value)            requires (is_virtual<K>);
    void           idle(DWORD clocks, RtiFlags = {});
    void           reset();                         // resets every TAP on the chain, whatever K is
    void           delay(std::chrono::microseconds);
    expected<void> flush();
    ...
};
```

A trailing `requires` on a member of a class template is enough to gate these
operations. Deducing `this` would only be needed for methods that consume the
object, and D1/D3 remove those.

---

## 3. Enumeration and the management guard

### 3.1 `HardwareInfo` and `Chain`

`AJI_HARDWARE` is used only inside `scan()`. Everything except `chain_id` is
copied into `HardwareInfo` (owned strings). `Chain` holds the id **and a
`HardwareInfo` by value**, so a `Chain` stays meaningful (for printing,
logging or matching) even after the library has reallocated its own copy of
the strings (F§2.4 row D, test 05).

```cpp
namespace aji {

struct HardwareInfo {                            // plain value
    DWORD                      persistent_id;    // the only key that survives reboots
    std::string                hw_name;
    std::optional<std::string> port, device_name, server;
    AJI_CHAIN_TYPE             chain_type;
    Features                   features;         // flag set over AJI_FEATURE
    std::string                server_version;   // from aji_get_hardware2's 3rd array
    bool is_dummy() const { return features.has(Feature::dummy); }   // "Unable to connect" etc.
};

class Chain {                                    // copyable
public:
    const HardwareInfo& info() const { return info_; }

    expected<std::vector<TapInfo>> cached_taps() const;     // read_device_chain without a lock

    // The only way to get a ChainLock (D3): it stays inside the scope.
    template<class F> auto locked(std::chrono::milliseconds timeout, F&& f) const;
private:
    AJI_CHAIN_ID  id_;
    HardwareInfo  info_;
    detail::Pin   pin_;                          // +1 on the process-wide pin count while this copy lives
};

// ---- management functions: may make the library free chains (F§2.4) ----
// Each one returns Error::chains_in_use if the pin count is non-zero.
expected<std::vector<Chain>> scan(std::chrono::milliseconds timeout = 2s);   // aji_get_hardware2
expected<Chain> find(std::predicate<const HardwareInfo&> auto&& pred,
                     std::chrono::milliseconds timeout = 2s);   // scan + exactly one match
expected<Chain> find_by_name(std::string_view, std::chrono::milliseconds);  // aji_find_hardware
expected<Chain> find_by_persistent_id(DWORD, std::chrono::milliseconds);     // aji_find_hardware
expected<void>  add_hardware(HardwareSpec const&);
expected<void>  remove(Chain&&);                 // allowed if this is the only pin
expected<void>  add_remote_server(std::string_view host, std::string_view password, Persist);
expected<void>  define_device(DeviceSpec const&);
expected<void>  undefine_device(DeviceSpec const&);
expected<std::vector<DeviceSpec>> defined_devices();
expected<void>  enable_remote_clients(bool, std::string_view password);

// ---- free at any time ----
void on_output(std::function<void(OutputLevel, std::string_view)>);
std::string last_error_info();                   // aji_get_error_info

}
```

`detail::Pin` is a process-wide atomic counter: incremented by the
constructors of `Chain`, `ChainLock` and `Open<K>`, decremented by their
destructors. It isn't an object you have to create or pass around, so the API
is plain free functions, as you asked. The state still exists, because the
library keeps it globally (`AJI_CLIENT::m_link`); the counter only reflects it.

### 3.2 Why the guard is needed

You asked for a stronger case, so here is the full list of what the library
frees and when (F§2.4 has the line references):

1. **Nothing happens in the background.** There are no threads in the client
   library. A chain object, its strings, or a whole server connection is only
   ever freed *inside one of our own `aji_*` calls*.
2. **But many calls can do it, not just `aji_get_hardware2`.** Any call that
   reaches `update_servers` or `refresh_hardware`: `aji_get_hardware(2)` and
   `aji_find_hardware` (both overloads) with a non-zero timeout,
   `aji_add_hardware`, `aji_remove_hardware`, `aji_add_remote_server`,
   `aji_define_device`/`undefine_device`/`get_defined_devices`,
   `aji_enable_remote_clients`/`get_remote_clients_enabled`. None of the
   scan, lock or open calls do (`lock`, `open_*`, `access_*`,
   `read_device_chain`, `get_nodes`, parameters).
3. **An unplugged cable on its own frees nothing.** The chain disappears at
   the next of those calls (`refresh_hardware`). Until then its id stays
   valid, and operations on it fail with ordinary errors.
4. **The trigger can be another program.** `update_servers` re-reads the
   shared config file (`~/.jtag.conf`) whenever its mtime changes, and deletes
   every server connection, with all its chains, whose entry vanished. So
   someone running `jtagconfig --removeserver`, or the Quartus Hardware Setup
   dialog, makes *our* next `aji_define_device` (say) free the chains we are
   holding. Test 06 shows this with ASan.
5. **It ignores locks.** `refresh_hardware` skips deletion when it can't take
   the connection mutex, so a locked chain is protected from (3), but only by
   accident. `update_servers` (4) deletes the connection regardless, and
   `~AJI_CLIENT` doesn't invalidate open devices
   (`jtag_client_link.cpp:640-656`; compare `disconnect()`, `:1301-1307`).
   Their `m_client` then dangles. (From reading the code only; not
   demonstrated, because it needs a lock.)
6. **Strings move even when the chain survives.** Every rescan calls
   `set_strings` on chains that are still present, which frees the old string
   block if any name changed: a renamed cable, or a server going from
   "Connecting ..." to connected. Same id, dangling `hw_name`/`server`
   (test 05).

So "don't call `aji_get_hardware2` while holding a chain" isn't enough. The
honest rule is "don't call any management function while holding anything",
and only a guard that covers all those functions makes it hold. A guard needs
a count of live handles, and that count is the state the old `Registry` was
modelling. Free functions plus a hidden counter keep the guarantee without the
object.

What the guard can't prevent: a stale chain caused by an *unplug* still makes
data-plane calls fail with `AJI_INVALID_CHAIN_ID` and similar errors. Those are
ordinary errors, not UB, because the object is only freed at the next
management call, and the guard blocks that while we hold the chain.

### 3.3 Usage

```cpp
using namespace std::chrono_literals;

// 1. List what's there. Dummy entries (unreachable servers) are real chains,
//    so filter them out explicitly.
{
    auto chains = aji::scan(2s);
    if (!chains) return log_error(chains.error());
    for (const aji::Chain& c : *chains | std::views::filter([](auto& c) { return !c.info().is_dummy(); }))
        std::println("{:08x}  {} [{}] on {}", c.info().persistent_id, c.info().hw_name,
                     c.info().port.value_or("-"), c.info().server.value_or("localhost"));
}   // chains destroyed: pins released

// 2. Pick one with your own filter (the "user-supplied filter" from the
//    original sketch). find() fails if there are no matches or more than one.
auto blaster = aji::find([](const aji::HardwareInfo& h) {
    return h.hw_name.starts_with("USB-Blaster") && !h.server;
});
if (!blaster) return log_error(blaster.error());   // Error::no_devices / Error::ambiguous

// 3. Lock the chain inside a scope, read the TAPs, open what we need.
auto hash = blaster->locked(10s, [&](aji::ChainLock& cl) -> aji::expected<Hash> {
    AJI_TRY(taps,  cl.scan());                                // read_device_chain(auto_scan=true)
    AJI_TRY(pos,   find_fpga(taps));
    AJI_TRY(nodes, cl.nodes(pos));
    AJI_TRY(hub,   cl.open_hub(pos, nodes.front().hub(0), aji::ClaimSet::sld_hub(), "app"));
    return cl.with(hub, aji::Pack::Manual, [](auto s) {      // s: Session<Kind::SldHub>, see §5
        s.overlay(HUB_IDENT);
        return s.dr({.length = 4, .read = {0, 4}, .batch = 32});   // Pending<Bits>
    });                       // lock handed back + batch flushed; Pending becomes a value or an Error
});                           // hub closed (lock already back with the chain), chain unlocked

// 4. Management calls are refused while handles are alive, instead of being UB.
auto again = aji::scan(2s);   // Error::chains_in_use while `blaster` exists
```

(`AJI_TRY` stands in for whatever early-return helper we pick for
`expected`; see §9.)

---

## 4. Lifetimes enforced by structure

```
aji::scan()/find() ──▶ Chain ──locked(f)──▶ ChainLock& ──open_*──▶ Open<K> (owned by caller code inside f)
                                                    │
                                                    └──with(open, g)──▶ Session<K> (prvalue, inside g)
```

- `Session<K>` borrows `Open<K>&` and only exists inside `with`, so an `Open`
  is never destroyed while locked (F§1.2: `close_device` on a locked open
  leaks the connection mutex).
- `Chain`, `ChainLock` and `Open<K>` each hold a pin, so no management call
  can free the chain object or server connection they point into (§3.2).
- `Open<K>` may outlive the `ChainLock` scope (`close_device` needs no chain
  lock, F§1.2).

---

## 5. Passing `Session` into the callback: a prvalue

Decision (yours): `with` passes a **prvalue** `Session`:

```cpp
template<Kind K, class F>
auto ChainLock::with(Open<K>& open, Pack pack, F&& f)
{
    ...
    Handback hb;                                             // ~Session reports into this
    auto r = std::forward<F>(f)(Session<K>{open, pack, *this, hb});
    // ~Session has run here, on every target (see below)
    ...
}
```

Why it's safe: whether a by-value parameter is destroyed when the callee
returns or at the end of the caller's full-expression is
implementation-defined ([expr.call]). But the full-expression is
`auto r = f(Session{...})`, *inside* `with`, so the handback has finished
before `with` runs its next statement, on any conforming compiler. I had
argued the opposite; you were right, and I've dropped that objection.

What it costs: the destructor can't return the handback's error, so `Session`
takes a `Handback&` owned by `with` and records the outcome there. `with`
reads it on the next line (§6).

Which lambda signatures work, checked with g++-14 and clang++ `-std=c++23`
(`tests/design/run.sh`):

| Parameter | Compiles | Scan ops available |
|---|---|---|
| `auto s` | yes | yes. **Recommended.** |
| `auto&& s` / `Session<K>&& s` / `Session<K> s` | yes | yes |
| `const auto s` / `const Session<K>& s` | yes | **no**: scan ops are non-const, so only read-only queries |
| `auto& s` / `Session<K>& s` | **no**: a prvalue doesn't bind to a non-const lvalue reference | — |
| `return s;` from the lambda | **no**: needs the deleted move constructor | — |

On `auto s` vs `const auto s`: recommend **`auto s`**. Scan operations change
library state (and the session's list of pending reads), so they're
deliberately non-const. Making them `const` would let `const auto s` compile,
but `const` would then promise nothing. `const auto s` is still useful for a
callback that only reads session info.

---

## 6. Handoff: scoped, and what happens when it fails

```cpp
template<Kind K, class F>
auto ChainLock::with(Open<K>& open, Pack pack, F&& f)
    -> expected<unwrap_pending_t<std::invoke_result_t<F, Session<K>>>>
{
    assert(!handed_away_);                       // runtime guard: see "Limits" below
    if (auto e = aji_unlock_chain_lock(id_, open.id(), to_c(pack)); e != AJI_NO_ERROR)
        return unexpected(Error{e});             // nothing moved; we still hold the chain lock
    handed_away_ = true;

    Handback hb;
    auto r = std::forward<F>(f)(Session<K>{open, pack, *this, hb});
    // ~Session ran by the end of the line above, even if f threw (§5); it
    // called aji_unlock_lock_chain and handled the outcomes below, recording
    // them in hb.
    handed_away_ = false;
    if (hb.lock_lost) lost_ = true;
    if (hb.error)     return unexpected(*hb.error);
    return settle(std::move(r));                 // Pending<T> → T (its batch is now flushed)
}
```

`~Session` has to deal with the three outcomes of `aji_unlock_lock_chain`
(F§3.3):

| Outcome | What the library state is | Wrapper action |
|---|---|---|
| `AJI_NO_ERROR` | chain locked | done |
| a deferred error returned by the early return at `jtag_client_open.cpp:587-588` | **open still locked**, error cleared | record the error, **retry the handoff once** (the flag is now clear) |
| error from `send_receive` (`:603-612`) | **nobody holds the lock** | record the error, then call `aji_lock_chain(id_, original_timeout)` to restore what `ChainLock` promises; if that fails, set `ChainLock` to *lost*: later operations return `Error::lock_lost` and the destructor skips `aji_unlock_chain` |

The *lost* state is the one place where we can't avoid a runtime state flag.
C++ can't make a type disappear when a library call fails.
(This recovery policy is a proposal, open item O3.)

### Limits of the scoped approach

> *"Scoped handoff might get convoluted if someone needs to move the lock a lot
> between different open ids, but it's probably the best compromise."*

Agreed. Two concrete cases:

1. **Recursive hub discovery.** `AJI_HUB::read_hub_information` hands the lock
   back mid-loop (`jtag_client_hub.cpp:275`) to recurse into a child hub
   (`:277`), which needs the chain lock to open it. The final handoff at
   `:291` then runs on an open that no longer holds the lock; the
   `AJI_NOT_LOCKED` it returns is ignored. With scopes, you collect the child
   addresses inside `with(hub, …)`, return them, and recurse from the
   `ChainLock` scope. That's more code, but the library's own manual version
   gets it wrong.
2. **Moving the lock directly between two opens** (`aji_unlock_lock`). Offered
   as `Session<A>::switch_to(Open<B>&, g)`: `g` gets a prvalue `Session<B>`, and the
   lock comes back to `A` when `g` returns. That's a nested scope; no new
   concept is needed.

What the scopes can't catch at compile time: the user's lambda can still
capture the `ChainLock&` by reference and call it while the lock is with the
open. The `handed_away_` flag turns that into an assertion (debug) or
`Error::lock_lost` (release). C++ has no linear types, so this hole stays.

If real code ever needs to move the lock back and forth a lot, the fallback is
an explicit runtime state machine (`Baton` type with `holder()`), checked on
every operation. Nothing in this repo needs that today.

---

## 7. Deferred results: `Pending<T>`

The problem in one line: under `AJI_PACK_MANUAL`, `aji_access_dr` returns
`AJI_NO_ERROR`, keeps a pointer to your buffer, and fills it at some later
flush, or never, if anything in the batch fails (F§3.1-3.2). `jtagconfig`
reads such buffers uninitialised (F§3.4).

```cpp
template<class T>
class Pending {                                  // move-only; issued by a Session
public:
    bool ready() const;                          // the session's flush checkpoint has passed
    expected<T> get() &&;                        // Error::not_flushed before the flush;
                                                 // the batch's error if the flush failed
private:
    std::shared_ptr<detail::Slot<T>> slot_;      // the session owns the read buffer; the slot outlives the session
};
```

- The **read buffer is owned by the wrapper** (`Slot`), so its address stays
  valid for as long as the library may write to it, and the caller never
  passes a raw pointer.
- `Session::flush()` and the handoff back mark every outstanding slot as
  done or failed. A failure poisons the whole batch, because the library
  reports only the first error of a batch and fills no buffers after one
  (`jtag_client_open.cpp:2151-2157`). Which scan in the batch failed is
  unknown; the library tracks `m_checkpoint` internally but doesn't expose it.
- With `Pack::Never`, and for reads with `Pack::Auto`, scans run immediately
  (F§3.1 table). Then `Pending` is ready on return and `get()` costs nothing.

**Optional, decision O4:** make the pack style a template parameter,
`Session<K, Pack::Never>`. Then `dr()` returns `expected<Bits>` directly in
immediate mode and `Pending<Bits>` only in deferred mode. That catches "read
before flush" at compile time instead of returning `Error::not_flushed`, at
the cost of one more template parameter on every signature.

---

## 8. File layout

```
include/aji/
  error.hpp      Error, error_category, expected<T> alias
  values.hpp     HardwareInfo, TapInfo, SldNodeInfo, SldHubAddr, ClaimSet, flag enums
  aji.hpp        namespace-level management functions (scan, find, add_remote_server, ...)
  chain.hpp      Chain, detail::Pin
  chain_lock.hpp ChainLock
  open.hpp       Kind, Open<K>
  session.hpp    Session<K>, Pending<T>, DrScan
  all.hpp        umbrella header
src/cpp/         the non-template parts (management functions, error_category, ChainLock)
tests/ub/        sanitizer demonstrations of the C API hazards (exists now)
tests/design/    compile checks for API-shape claims (exists now)
tests/wrapper/   wrapper tests (to come; needs O5 for the lock and deferred paths)
```

Left out of the wrapper: every `AJI_CLAIM` (v1) overload, the 4-argument and
5-argument `DWORD*` forms of `aji_get_nodes`, the two `aji_open_node`
overloads that take an `idcode`, `aji_get_hardware` (use `aji_get_hardware2`),
`aji_get_server_version`. Internally we use the `int`-workaround form of
`aji_get_nodes` (`aji.h:1835`), since the wrapper owns the buffers anyway.

---

## 9. Open items for sign-off

| # | Question | My recommendation |
|---|---|---|
| O1 | Error style: `std::expected` everywhere, or exceptions with `expected` at the edges? | `expected`. Most JTAG failures (timeouts, `CHAIN_IN_USE`) are normal outcomes, not exceptional ones. |
| O2 | A management call while handles are alive: an error (as written), or an assertion? | Error in release builds, assertion in debug builds. |
| O3 | When the handoff back fails and nobody holds the lock: re-lock automatically (as written) or always mark the lock lost? | Re-lock with the original timeout. It keeps `ChainLock`'s promise in the common case. |
| O4 | Pack style as a template parameter (§7)? | Yes, but only after the plain version works. |
| O5 | Write a minimal fake jtagd for the lock and deferred-path tests (F§3.5)? | Yes. Without it, D7/D8 are untested apart from on real hardware. |
| O6 | The library bugs (F§4): patch them in this repo, or work around them in the wrapper? | Patch. Upstream is archived and doesn't accept PRs, so this fork is where fixes live. Each is a few lines, and tests 03/04 already cover two of them. |
| O8 | Protocol drift with Intel's jtagd (F§5)? | Nothing to do now. Add a `server_protocol_version()` query and a test against whatever jtagd we use, so a break shows up as one failing test rather than odd behaviour. |
| O7 | Small facts to check on real hardware or against the server source: the TAP position direction (§2), and whether `access_ir` on a node/hub open is rejected. | Check before freezing the `requires` clauses. |
