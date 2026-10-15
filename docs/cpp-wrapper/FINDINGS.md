# libaji_client: behaviour the C++ wrapper must respect

These facts come from reading the client implementation in `src/jtag/`, not
only `src/h/aji.h`. Where the two disagree, the code wins. Line numbers refer
to commit `5d9c782` (the "Archiving Notice" commit on `main`).

The sections, in the order the design depends on them:

1. [Locking: one lock per server connection, tied to a thread](#1-locking)
2. [Lifetimes: chain ids and hardware strings belong to the library](#2-lifetimes) (shown with ASan in `tests/ub/`)
3. [Deferred execution: results and errors arrive later](#3-deferred-execution) ("point 4" in the first review)

At the end: [library bugs found along the way](#4-library-bugs-found-along-the-way)
and [drift with Intel's jtagd](#5-drift-with-intels-jtagd).

---

## 1. Locking

### 1.1 The single lock is per server connection

Each server connection (`AJI_CLIENT`, one for the local jtagd and one per
remote server) has a single, non-recursive mutex:

```cpp
// jtag_client_link.h:282, 289-291
JTAG_MUTEX m_link_mutex;
inline bool AJI_CLIENT::try_claim_link(DWORD ms_timeout)
{   return m_link_mutex.try_claim(ms_timeout); }
```

On Linux `try_claim` is `pthread_mutex_timedlock` (`jtag_platform_unix.cpp:162-193`).

Both lock functions take that mutex and **keep it until unlock**:

| Function | Takes the mutex | Marks locked | Releases |
|---|---|---|---|
| `aji_lock_chain` → `AJI_CHAIN_JS::lock` | `jtag_client_chain.cpp:835` | `:853` | `unlock`, `:884` |
| `aji_lock` → `AJI_OPEN_JS::lock` | `jtag_client_open.cpp:323` | `locked_ok()`, `:274` | `unlock`, `:408-412` |

Consequences:

- **Only one lock can be held per server connection.** Locking a TAP while its
  chain is locked waits out the timeout in `try_claim_link`, then returns
  `AJI_SERVER_ACTIVE`. It doesn't deadlock; it fails. Two chains on the *same*
  server also can't be locked at the same time. Chains on different servers
  can.
- **The lock is tied to the thread.** Unlocking a pthread mutex from a thread
  that doesn't own it is undefined behaviour. So a C++ object that owns a lock
  must not move to another thread, which in practice means it must not be
  movable at all.
- **While you hold the lock, no other thread in your process can talk to that
  server**, not even for operations that need no lock (`refresh_hardware` only
  waits 100 ms, `jtag_client_link.cpp:1387`, then skips the refresh).

The three `aji_unlock_*lock*` functions exist to **pass the one lock between
holders atomically**:

| Function | From → to | Implementation |
|---|---|---|
| `aji_unlock_chain_lock(chain, open, pack)` | chain → open | `jtag_client_open.cpp:494` |
| `aji_unlock_lock_chain(open, chain)` | open → chain | `jtag_client_open.cpp:556` |
| `aji_unlock_lock(open_a, open_b)` | open → open | `jtag_client_open.cpp:438` |

The library's own code always works this way: lock the chain, open, hand the
lock to the open, scan, hand it back, close. See `AJI_HUB::read_hub_information`
(`jtag_client_hub.cpp:189` open, `:205` hand to the hub, `:291` hand back) and
`print_design_hash` in `jtagconfig.cpp` (`:837` open, `:842` hand over, `:858`
hand back, `:860` close).

### 1.2 Which functions need which lock

Taken from the `m_locked` checks in the code.

| Needs | Functions |
|---|---|
| no lock | `get_hardware2`, `find_hardware`, `print_hardware_name`, `add_hardware`/`remove_hardware`, remote-server functions, `define_device`/`undefine_device`/`get_defined_devices`, `register_output_callback`, `get_error_info`, `read_device_chain` (returns the cached result only: `jtag_client_chain.cpp:957`) |
| **chain** locked | `read_device_chain` (rescans), `get_parameter`/`set_parameter` (`:1212`, `:1241`, `:1279`), `open_device` (`:1379`), `open_entire_device_chain` (`:1572`), `get_nodes`, `open_node`, `open_hub` |
| **open id** locked | `access_ir`, `access_dr`, `run_test_idle`, `test_logic_reset`, `delay`, `flush` (a no-op when not locked: `jtag_client_open.cpp:1948`), `access_overlay` |
| open id **un**locked | `close_device` |

Where the header is wrong:

- `aji.h` says `open_entire_device_chain` requires the chain *not* locked. The
  code returns `AJI_NOT_LOCKED` unless it is locked (`jtag_client_chain.cpp:1572`).
- `close_device` on a locked open id returns `AJI_LOCKED`
  (`jtag_client_open.cpp:633-634`) **but still runs `delete this`** (`:679`)
  without releasing the connection mutex the lock holds. Every later call to
  that server from this process then fails with `AJI_SERVER_ACTIVE`. The
  wrapper must guarantee an open is unlocked before it is closed.

---

## 2. Lifetimes

### 2.1 `AJI_CHAIN_ID` is a raw pointer to a library-owned object

`AJI_CHAIN_ID` is `AJI_CHAIN *`, and `AJI_CHAIN` is a polymorphic class
(`jtag_client_aji.h:81`). The C entry points call a virtual method on it
directly, with no validity check:

```cpp
// jtag_client_aji.cpp:195-202 (same pattern for every chain_id function)
AJI_ERROR AJI_API aji_print_hardware_name(AJI_CHAIN_ID chain_id, char * hw_name, DWORD hw_name_len)
{
    return chain_id->print_hardware_name(hw_name, hw_name_len);   // vptr load
}
```

`AJI_CHAIN_JS::valid()` exists, but it runs *inside* the virtual call, after
the vptr has already been read from the object.

The library deletes chain objects:

- on any rescan where the server no longer reports that hardware
  (`AJI_CLIENT::refresh_hardware`, `jtag_client_link.cpp:1446-1456`). This
  runs inside every `aji_get_hardware*` call with a non-zero timeout, via
  `update_hardware` (`jtag_client_chain.cpp:133`). Unplugging a cable and
  calling `aji_get_hardware2` again is enough.
- when the connection to a remote server is removed (`aji_remove_hardware` on
  one of its chains → `remove_remote_server` → `delete this` →
  `~AJI_CLIENT` deletes its chains, `jtag_client_link.cpp:643`).
- after `aji_remove_hardware` on a local chain (`jtag_client_chain.cpp:797`;
  the library's own comment says *"Beware: Deletes 'this'"*).

After that, **using the old id is a heap use-after-free**, not an
`AJI_INVALID_CHAIN_ID` error. Shown by `tests/ub/01_stale_chain_id.cpp`:

```
aji_remove_hardware                      -> 0
==ERROR: AddressSanitizer: heap-use-after-free on address 0x510000000040
READ of size 8 at 0x510000000040 thread T0
    #0 in aji_print_hardware_name(AJI_CHAIN*, char*, unsigned int) src/jtag/jtag_client_aji.cpp:199
freed by thread T0 here:
    #1 in AJI_CHAIN_JS::~AJI_CHAIN_JS() src/jtag/jtag_client_chain.cpp:605
    #2 in AJI_CLIENT::~AJI_CLIENT() src/jtag/jtag_client_link.cpp:643
    ...
```

The test uses the remote-server removal path because it is deterministic and
needs no hardware. The rescan path ends in the same `delete` of the chain
object, but needs a cable that disappears.

### 2.2 The strings in `AJI_HARDWARE` alias the chain object

`aji_get_hardware2` doesn't copy strings. It slices the library's chain object
into your array:

```cpp
// jtag_client_chain.cpp:143
*hardware_list++ = *(*j).m_chain;
```

So `hw_name`, `port`, `device_name` and `server` point into storage owned by
that chain object (`m_hw_strings`, freed at `jtag_client_chain.cpp:604-605`)
and die with it. Shown by `tests/ub/02_dangling_hardware_strings.cpp`
(`heap-use-after-free` in `strlen` on `hw.server`).

**Consequence:** copy everything out of `AJI_HARDWARE` into owned
`std::string`s immediately. Keep `chain_id` separately, as a handle whose
validity depends on nobody re-enumerating (see `DESIGN.md` §3).

### 2.3 What the caller owns

- The server-version strings returned by `aji_get_hardware2` are
  `new char[]` (`jtag_client_chain.cpp:150`, `:155`). The caller must
  `delete[]` them; LeakSanitizer reports them otherwise.
- `AJI_OPEN_ID` is created by `open_device`/`open_node`/`open_hub`/
  `open_entire_device_chain` and destroyed by `close_device`. `close_device`
  runs `delete this` even when it returns an error (`jtag_client_open.cpp:679`).
  That's single ownership, so a move-only RAII type is the right fit.

### 2.4 When the library frees chains, strings and server connections

There are **no threads** in the client library (no `pthread_create` in
`src/jtag/`), so everything below happens synchronously, inside one of the
caller's own `aji_*` calls. The table lists every place that frees one of
the objects a caller can hold a pointer into.

| | What is freed | Code | Reached from |
|---|---|---|---|
| A | Chains the server no longer reports (cable unplugged) | `refresh_hardware`, `jtag_client_link.cpp:1446-1456` | `aji_get_hardware(2)` and both `aji_find_hardware` with timeout > 0 (`update_hardware`, `jtag_client_chain.cpp:133`, `:205`, `:414`), `aji_add_hardware` (`:757`), `aji_remove_hardware` (`:797`) |
| B | Whole server connections and all their chains, when that server's `RemoteN` entry vanished from or changed in the config file | `update_servers`, `jtag_client_link.cpp:196-223` → `~AJI_CLIENT`, `:640-643` | everything that calls `update_servers`: `update_hardware` (as A), `local_server()` (`aji_add_hardware`; `aji_get_hardware*` when no chains are found, `jtag_client_chain.cpp:173`; `get_server_version_info`/`get_server_path`; `aji_enable_remote_clients`/`get_remote_clients_enabled` via `check_remote_clients_possible`, `jtag_client_link.cpp:466`, `:498`), `aji_add_remote_server` (`:341`-`:404`), `aji_define_device`/`undefine_device` (`:1647`), `aji_get_defined_devices` (`:1697`) |
| C | A remote server's connection and chains | `remove_remote_server`, `jtag_client_link.cpp:657-704` | `aji_remove_hardware` on any chain of that server |
| D | Only the strings of a chain that still exists, under the same id | `set_strings`, `jtag_client_chain.cpp:651-663`, called by `refresh_chain`, `jtag_client_link.cpp:1593-1599` | every rescan (as A), whenever any of the chain's names changed: a renamed cable, or a server going from "Connecting ..." to connected |

What this means in practice:

- **An unplugged cable on its own frees nothing.** The chain object is freed
  at the next call in row A. Until then, calls on it fail with ordinary
  errors.
- **Row B can be triggered by another process.** The config file
  (`$QUARTUS_JTAG_CLIENT_CONFIG`, default `~/.jtag.conf`,
  `jtag_configuration_file.cpp:114-128`) is re-read whenever its mtime or
  ctime changes (`:178-182`). It's the same file `jtagconfig` and the Quartus
  Programmer edit. Someone running `jtagconfig --removeserver` makes our next
  `aji_define_device`, say, free the chains we hold. Test
  `06_config_edited_by_other_process.cpp` shows this: the chain is freed
  inside `update_servers`, and the next use is a heap use-after-free.
- **Row B ignores locks.** Row A skips deletion when it can't take that
  connection's mutex (`m_destroy = mutex_claimed`,
  `jtag_client_link.cpp:1387-1393`), so a chain we have locked survives row A
  by accident. Row B takes no mutex at all.
- **Row B leaves open devices dangling.** `~AJI_CLIENT` closes the socket but
  doesn't call `disconnect()`, which is what invalidates the connection's open
  devices (`AJI_OPEN_JS::invalidate`, `jtag_client_link.cpp:1301-1307`). Each
  `AJI_OPEN_JS` keeps its `m_client` pointer (`jtag_client_open.h:232`) to
  the freed connection. *From reading the code only; not demonstrated, because
  it needs a server that grants a lock.*
- **Row D means even a chain id that is still valid can have dangling
  strings.** Test `05_strings_freed_by_plain_rescan.cpp`: same id before and
  after, old `server` pointer freed.
- **A quirk:** the deletion loop in row B never examines `m_link[0]`
  (`for (it = end()-1; it != begin(); )`, `jtag_client_link.cpp:203`). In
  normal use that slot is the local jtagd, so remote servers are all
  examined. With `QUARTUS_JTAG_CLIENT_NO_LOCAL_SERVER`, the first remote
  server is never removed this way.

---

## 3. Deferred execution

This is "point 4" from the first review. With `AJI_PACK_MANUAL` (and
`AJI_PACK_STREAM`), **scan calls return `AJI_NO_ERROR` before they have run**.
Results land in your buffers later, and errors are reported later, by a
different call.

### 3.1 How an `access_dr` gets deferred

```cpp
// jtag_client_open.cpp:1099-1103: the read FIFO keeps *your* pointer
RXRAWFIFO * rxbits = NULL;
if (read_length > 0)
    rxbits = new RXRAWFIFO(m_client, rx_bytes * batch, read_bits);
```
```cpp
// jtag_raw_fifo.cpp:179: no copy, just stores the pointer
: RXFIFO(link), m_data(buffer), ...
```
```cpp
// jtag_client_open.cpp:1173-1187: deferred path
if (defer)
{
    assume_success(completion_fn, handle, total_tx_bytes);
    if (rxbits != NULL)
        expect_output(rxbits);      // filled when the response packet arrives
    ...
    return AJI_NO_ERROR;            // nothing has executed yet
}
```

The same applies to the bit-vector form of `access_ir` (`:857`, `:899-906`)
and to `access_ir(DWORD instr, DWORD * captured_ir)`, whose `captured_ir`
pointer is stored and written later by `deferred_response_access_ir`
(`:764`, `:686-701`).

Whether a call is deferred is decided in `get_txmessage`
(`jtag_client_open.h:318-370`):

| Pack style | Calls that return data (`read_bits`, `captured_ir`) | Write-only calls |
|---|---|---|
| `AJI_PACK_NEVER` | run immediately | run immediately |
| `AJI_PACK_AUTO` | run immediately (`output_n > 0` turns deferral off, `jtag_client_open.h:348-350`; `access_ir(DWORD)` also forces it off, `jtag_client_open.cpp:749-750`) | **deferred** |
| `AJI_PACK_MANUAL` / `STREAM` | **deferred** | **deferred** |

Deferral is also turned off for writes of 1024 bytes or more (`jtag_client_open.h:354`)
and for scans to registers that weren't claimed (`jtag_client_open.cpp:1128-1130`).
The queue is flushed early when its fixed-size action buffers fill up
(`jtag_client_open.h:324-336`, `:366-371`). So a result can arrive *earlier*
than the flush, but the only *guarantee* is: after `aji_flush`, `aji_unlock`,
or a successful handoff.

Write data is copied when deferred (`TXRAWFIFO::copy_data`,
`jtag_raw_fifo.cpp:132-146`), **except** in `STREAM` mode with at least 1024
bytes (`jtag_client_open.cpp:1132-1141`), where the caller's buffer is
used as-is and the code calls `send_continue()` (`:1187-1188`). I haven't
verified that `send_continue` has finished reading the buffer by the time
`access_dr` returns. The wrapper should treat it as borrowed until the flush.

### 3.2 Where results and errors arrive

`receive_response` (`jtag_client_open.cpp:2119-2197`) processes the replies
to a batch:

- It runs each deferred completion function (`:2135-2149`). For
  `captured_ir` this is where the `DWORD` is written.
- **It waits for the read FIFOs only if no error occurred** (`:2151-2157`),
  then deletes them (`:2160-2161`). If anything in the batch failed, your
  read buffers may be partly written or untouched.
- It returns the first error of the batch. That error comes back from
  whichever call drained the queue: `aji_flush`, `aji_unlock`, or a handoff.
  It doesn't come from the `access_dr` that caused it.

### 3.3 The handoff back can fail and drop the lock

`aji_unlock_lock_chain` (open → chain) is not "unlock and always succeed":

```cpp
// jtag_client_open.cpp:587-588: a deferred error is pending
if (m_defer_error != AJI_NO_ERROR)
    return pending_error();          // returns early, the OPEN STAYS LOCKED
                                     // (pending_error clears the flag, :2200-2206)
```
```cpp
// jtag_client_open.cpp:603-612: the batch, including the handoff, is sent
error = send_receive(&rx);           // first error of the whole batch
m_locked = false;                    // the open is unlocked whatever happened
if (error == AJI_NO_ERROR)
    lock_chain->set_locked(true);
else
    m_client->release_link();        // ...and on error the CHAIN IS NOT LOCKED
return error;
```

So when a deferred scan fails, the handoff back either:

- (a) leaves the lock on the open (`m_defer_error` path), or
- (b) leaves **nobody** holding it: neither the open nor the chain is locked.

On the server side, `receive_response` sends an extra unlock to resync
(`:2184-2192`).

### 3.4 The bug this causes in `jtagconfig`

`print_design_hash` (`jtagconfig.cpp:827-885`):

```cpp
842  err = aji_unlock_chain_lock(chain_id, hub_id, AJI_PACK_MANUAL);   // MANUAL: everything below is deferred
843
844  unsigned char minor_buff[1];                                       // uninitialised
845  unsigned char ident_buff[32];                                      // uninitialised
...
848      err = aji_access_overlay(hub_id, HUB_MINORVERSION, NULL);       // returns NO_ERROR (queued)
850      err = aji_access_dr(hub_id, 4, ..., minor_buff, 1);              // returns NO_ERROR (queued, keeps &minor_buff)
853      err = aji_access_overlay(hub_id, HUB_IDENT, NULL);
855      err = aji_access_dr(hub_id, 4, ..., ident_buff, 32);
857  // Pass lock from device to chain atomically, flush buffer
858  aji_unlock_lock_chain(hub_id, chain_id);                           // the batch's error comes back HERE; it is ignored
859
860  aji_close_device(hub_id);
...
865  if ((minor_buff[0] & 1) == 0)                                      // reads a buffer that may never have been filled
876      ptr += snprintf(..., ident_buff[12+i] & 0xF);
```

If the server rejects any of the queued scans:

1. `err` is still `AJI_NO_ERROR`, because every call at 848-855 only queued
   work.
2. The real error is returned at 858 and thrown away.
3. Per §3.3, the chain is then **not** locked. When the caller later runs
   `aji_unlock_chain(chain)` (`jtagconfig.cpp:692`), it returns
   `AJI_NOT_LOCKED`, and the parameter reads in between fail.
4. `minor_buff`/`ident_buff` are read uninitialised at 865/876 and printed as
   a design hash.

The same pattern appears in `AJI_HUB::read_hub_information`: the result of the
handoff at `jtag_client_hub.cpp:291` is ignored. Also, when a bridge is found,
the lock is handed back mid-loop at `:275` before recursing at `:277`, so the
handoff at `:291` then runs on an open that no longer holds the lock (and
returns `AJI_NOT_LOCKED`, also ignored).

**For the wrapper this means:**

- A deferred scan must not hand back a plain value or a plain `expected<T>`.
  Its result only exists after the batch has been flushed successfully.
- Read buffers must be owned by the session (or by a `Pending<T>` it issues),
  not borrowed from the caller.
- The handoff back is fallible, and on failure the wrapper has to deal with
  "nobody holds the lock" (see `DESIGN.md` §6).

### 3.5 Not demonstrated by tests

The deferred-read hazards need a server that accepts the lock and then fails a
queued scan. `tests/ub/` doesn't include a fake jtagd. A small server that
speaks enough of the protocol (`jtag_protocol.cpp`, `jtag_message.h`) to fail
an `ACCESS_DR` would let us turn §3.3-3.4 into tests. It's listed in
`DESIGN.md` as an open item.

---

## 4. Library bugs found along the way

All found by the same sanitizer build.

| Bug | Where | Test |
|---|---|---|
| Any server given as a dotted IPv4 address (`"192.168.1.5:1309"`) overflows a heap buffer on 64-bit Linux: 4 bytes are allocated, then an 8-byte `unsigned long` is stored into them. | `TCPCLIENT::lookup`, `jtag_tcpclient.cpp:126` and `:133` | `03_dotted_ip_overflow.cpp` |
| A temporary server's info block is allocated with `new[]` and freed with `delete`. Removing any temporary remote server is UB. | `jtag_client_link.cpp:442` (allocated) / `:679` (freed) | `04_temp_server_delete_mismatch.cpp` |
| With `QUARTUS_JTAG_CLIENT_NO_LOCAL_SERVER` set, any `aji_get_hardware*` that finds no valid chain calls `local_server()->connected()` on `NULL`: segfault. That happens with no remote servers configured (every call), or with timeout 0 before any server has been contacted. Without that variable the local server always exists, so normal setups can't hit it. | `jtag_client_chain.cpp:173` | none (seen while writing test 05 and the wrapper examples) |
| The config parser calls `token(NULL, 0)`, which computes `NULL + 0 - 1`. UB, reported by UBSan on every config load with a key/value; harmless in practice. | `jtag_configuration_file.cpp:227` (call), `:275` (arithmetic) | shows up in the logs of tests 06 and others |

Each is a few lines to fix. Upstream is archived and doesn't accept PRs, so
fixes would live in this fork (`DESIGN.md` §9, O6).

---

## 5. Drift with Intel's jtagd

Upstream (`intel/libaji_client`) is archived, so this client won't follow
changes in jtagd. How exposed that leaves us, from the connection handshake
(`AJI_CLIENT::continue_connect`, `jtag_client_link.cpp:1000-1075`):

- **Protocol versions are negotiated.** The server announces its version.
  The client sends `USE_PROTOCOL_VERSION min(server, 13)`
  (`AJI_CURRENT_VERSION`, `jtag_message.h:86`; `jtag_client_link.cpp:1061-1068`).
  A newer jtagd that still speaks version 13 works unchanged; the client
  already branches on older versions (`version_ok(...)`,
  `server_version >= 5`/`>= 11` in `jtag_client_open.cpp`).
- **What would break it:**
  - a jtagd that drops version 13 (connection state `BROKEN`);
  - a change of signature string (`BADVERSION`, `:1008-1014`);
  - authentication other than MD5 challenge/response (`authtype != 0` →
    `AUTHFAIL`, `:1054-1055`).
- **What we'd miss without breaking:** anything Intel adds above version 13,
  such as new message types or hardware features.

Speculation: Intel has kept jtagd backward-compatible across many Quartus
releases, because old Quartus installs and third-party tools depend on it.
So a silent break seems less likely than gradually missing new features. That
is a guess, not something I've checked against jtagd releases. The cheap
insurance is a single connection test against the jtagd version we actually
use (`DESIGN.md` O8).
