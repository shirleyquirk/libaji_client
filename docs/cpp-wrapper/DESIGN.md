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
| D5 | `AJI_HARDWARE` is **discarded after copying**: `HardwareInfo` holds owned strings, `Chain` holds the id | The strings and the id are library-owned and can be freed under us (F§2.1, F§2.2). |
| D6 | **One `Registry`** per process owns enumeration. A rescan is refused while any `Chain` handle is alive. | Rescanning frees chain objects; using a stale id is a use-after-free (F§2.1, test 01). |
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
| `Registry` | `aji_get_hardware2` and the process-wide functions | process singleton | neither |
| `Chain` | `AJI_CHAIN_ID` + `HardwareInfo` | **borrowed** from the `Registry` | copyable |
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

## 3. Enumeration: `HardwareInfo` vs `Chain`

To your question: **yes, we discard `AJI_HARDWARE`.** Everything in it except
`chain_id` is copied into `HardwareInfo` (owned strings). `chain_id` goes into
`Chain`, next to the `HardwareInfo`, and `Chain` is what we hand out. The
strings have to be copied because they alias the chain object (F§2.2,
test 02). The id has to be guarded because it is a raw pointer to that object
(F§2.1, test 01).

```cpp
class Registry {
public:
    static Registry& instance();                 // aji_* state is process-global anyway

    // Calls aji_get_hardware2 (growing the array on AJI_TOO_MANY_DEVICES),
    // copies everything out, delete[]s the version strings (F§2.3).
    // Returns Error::chains_in_use if any Chain/ChainLock/Open is alive,
    // because the rescan can free their chain objects (F§2.1).
    expected<std::span<const Chain>> scan(std::chrono::milliseconds timeout = 5s);

    std::span<const Chain> chains() const;       // last scan, no I/O

    // Shortcuts over chains()
    expected<Chain> find(std::predicate<const HardwareInfo&> auto&& pred) const;   // exactly one match
    expected<Chain> by_persistent_id(DWORD id) const;    // the only key that survives reboots
    expected<Chain> by_name(std::string_view name, std::chrono::milliseconds timeout);   // aji_find_hardware

    // Process-wide functions (no chain involved)
    expected<void> add_remote_server(std::string_view host, std::string_view password, Persist);
    expected<void> remove(Chain&&);              // aji_remove_hardware; frees the chain object
    void on_output(std::function<void(OutputLevel, std::string_view)>);
    DeviceDatabase& devices();                   // define/undefine/get_defined_devices
};

class Chain {                                    // cheap, copyable, non-owning
public:
    const HardwareInfo& info() const;
    bool is_dummy() const;                       // AJI_FEATURE_DUMMY: "Unable to connect", etc.

    expected<std::vector<TapInfo>> cached_taps() const;     // read_device_chain without a lock

    // The only ways to get a ChainLock (D3): it stays inside the scope.
    template<class F> auto locked(std::chrono::milliseconds timeout, F&& f) const
        -> expected<std::invoke_result_t<F, ChainLock&>>;
private:
    AJI_CHAIN_ID id_;
    const HardwareInfo* info_;
    Registry::Pin pin_;                          // counts live handles; scan() checks it
};
```

### Usage

```cpp
using namespace std::chrono_literals;
auto& aji = aji::Registry::instance();

// 1. Scan once. Dummy entries for servers we can't reach are real chains
//    (tests/ub uses one), so filter them out explicitly.
auto chains = aji.scan(2s);
if (!chains) return log_error(chains.error());

for (const aji::Chain& c : *chains | std::views::filter(std::not_fn(&aji::Chain::is_dummy)))
    std::println("{:08x}  {} [{}] on {}", c.info().persistent_id, c.info().hw_name,
                 c.info().port, c.info().server.value_or("localhost"));

// 2. Pick one with your own filter, the "user-supplied filter" from the
//    original sketch. find() fails if there are no matches or more than one.
auto blaster = aji.find([](const aji::HardwareInfo& h) {
    return h.hw_name.starts_with("USB-Blaster") && h.server == std::nullopt;
});
if (!blaster) return log_error(blaster.error());   // Error::no_devices / Error::ambiguous

// 3. Lock the chain inside a scope, read the TAPs, open what we need.
auto result = blaster->locked(10s, [&](aji::ChainLock& cl) -> aji::expected<Hash> {
    AJI_TRY(taps, cl.scan());                                 // read_device_chain(auto_scan=true)
    AJI_TRY(pos,  find_fpga(taps));
    AJI_TRY(nodes, cl.nodes(pos));
    AJI_TRY(hub,  cl.open_hub(pos, nodes.front().hub(0), aji::ClaimSet::sld_hub(), "app"));
    return cl.with(hub, aji::Pack::Manual, [](aji::Session<aji::Kind::SldHub>& s) {
        s.overlay(HUB_IDENT);
        return s.dr({.length = 4, .read = {0, 4}, .batch = 32});   // Pending<Bits>
    });                       // lock handed back + batch flushed; Pending turns into a value or an Error
});                           // hub closed (lock already back with the chain), chain unlocked

// 4. Rescanning while handles are alive is refused, not UB.
auto again = aji.scan(2s);    // Error::chains_in_use while `blaster` exists
```

(`AJI_TRY` stands in for whatever early-return helper we pick for
`expected`; see §9.)

---

## 4. Lifetimes enforced by structure

```
Registry ──borrows──▶ Chain ──locked(f)──▶ ChainLock& ──open_*──▶ Open<K> (owned by caller code inside f)
                                               │
                                               └──with(open, g)──▶ Session<K>& (inside g)
```

- `Session<K>` borrows `Open<K>&` and only exists inside `with`, so an `Open`
  is never destroyed while locked (F§1.2: `close_device` on a locked open
  leaks the connection mutex).
- `Open<K>` keeps a `Registry::Pin`, so the chain object it points into can't
  be rescanned away under it.
- `Open<K>` may outlive the `ChainLock` scope (`close_device` needs no chain
  lock, F§1.2), but not the `Registry` pin.

---

## 5. Passing `Session` into the callback: `&` vs `&&`

> *"Session type is non-copy non-move? Even without a move constructor we can
> pass into the lambda as rref, right?"*

Yes. Binding a reference never needs a move constructor. `f(std::move(s))` is
only a cast, and `f(Session{...})` binds a materialised temporary. Checked
with g++-14 `-std=c++23` (reproduce with `tests/design/run.sh`):

```cpp
struct Session { /* copy and move constructors deleted */ ~Session(); };

template<class F> auto with_named  (F&& f) { Session s{1}; return f(s); }
template<class F> auto with_rref   (F&& f) { Session s{1}; return f(std::move(s)); }
template<class F> auto with_prvalue(F&& f) { return f(Session{2}); }

with_named  ([](Session&  s) { ... });   // OK
with_named  ([](auto&     s) { ... });   // OK
with_rref   ([](Session&& s) { ... });   // OK: no move constructor needed
with_prvalue([](Session   s) { ... });   // also compiles: guaranteed elision builds s in place
with_named  ([](Session   s) { ... });   // error: use of deleted copy constructor   <- what we want
with_named  ([](Session&& s) { ... });   // error: lvalue doesn't bind to Session&&
```

**Recommendation: build the `Session` as a named local and pass `Session<K>&`.**

- `&&` normally tells the callee "you may take this", but nobody can take a
  non-movable object: it can't be stored, moved, or returned. So `&&` adds
  nothing that `&` doesn't already say.
- The prvalue form is a trap. It lets a by-value parameter compile, and then
  the `Session` destructor (which does the handoff back) runs when the
  *callee's parameter* is destroyed. Whether that happens inside the callee or
  at the end of the caller's full-expression is implementation-defined.
  Passing a named local as an lvalue makes `[](Session s)` a compile error, so
  the handoff point stays fixed inside `with`.

---

## 6. Handoff: scoped, and what happens when it fails

```cpp
template<Kind K, class F>
auto ChainLock::with(Open<K>& open, Pack pack, F&& f)
    -> expected<unwrap_pending_t<std::invoke_result_t<F, Session<K>&>>>
{
    assert(!handed_away_);                       // runtime guard: see "Limits" below
    if (auto e = aji_unlock_chain_lock(id_, open.id(), to_c(pack)); e != AJI_NO_ERROR)
        return unexpected(Error{e});             // nothing moved; we still hold the chain lock
    handed_away_ = true;

    Session<K> s{open, pack};
    auto r = std::invoke(f, s);                  // may throw: the destructor below still hands back

    auto back = s.hand_back_to(*this);           // see the three outcomes below
    handed_away_ = false;
    if (!back) return unexpected(back.error());
    return settle(std::move(r));                 // Pending<T> → T (its batch is now flushed)
}
```

`hand_back_to` has to deal with the three outcomes of `aji_unlock_lock_chain`
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
   as `Session<A>::switch_to(Open<B>&, g)`: `g` gets a `Session<B>&`, and the
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
  registry.hpp   Registry, Chain, DeviceDatabase
  chain_lock.hpp ChainLock
  open.hpp       Kind, Open<K>
  session.hpp    Session<K>, Pending<T>, DrScan
  aji.hpp        umbrella header
src/cpp/         the non-template parts (Registry, error_category, ChainLock)
tests/ub/        sanitizer demonstrations of the C API hazards (exists now)
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
| O2 | `Registry::scan` while handles are alive: an error (as written), or an assertion? | Error in release builds, assertion in debug builds. |
| O3 | When the handoff back fails and nobody holds the lock: re-lock automatically (as written) or always mark the lock lost? | Re-lock with the original timeout. It keeps `ChainLock`'s promise in the common case. |
| O4 | Pack style as a template parameter (§7)? | Yes, but only after the plain version works. |
| O5 | Write a minimal fake jtagd for the lock and deferred-path tests (F§3.5)? | Yes. Without it, D7/D8 are untested apart from on real hardware. |
| O6 | The two library bugs (F§4): patch the library in this repo, or work around them in the wrapper (resolve dotted IPs ourselves; never remove temporary servers)? | Patch. Both are one-line fixes, and the tests already exist to confirm them. |
| O7 | Small facts to check on real hardware or against the server source: the TAP position direction (§2), and whether `access_ir` on a node/hub open is rejected. | Check before freezing the `requires` clauses. |
