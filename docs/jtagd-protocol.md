# jtagd wire protocol (as spoken by the AJI client)

Reconstructed from the client side in `libaji_client` (`src/jtag/jtag_message.h`,
`jtag_tcplink.cpp`, `jtag_client_link.cpp`, `jtag_client_chain.cpp`,
`jtag_client_open.cpp`). No server-side source is public. Everything below is
what a server must do for the reference client to be happy, and
`test/mock_jtagd` implements it (see `docs/mock-jtagd.md`). Items marked
**[inferred]** are not visible in client code; the mock picks a plausible
behaviour for each, but they need confirming against a real jtagd (see
"Validation" at the end).

All integers are **big-endian**. `int` = u32, `long` = u64.

## 1. Transport framing (TCPLINK)

TCP, default port **1309**. Both directions carry a stream of packets:

```
+--------+--------+=====================+
| hdr hi | hdr lo | payload (len bytes) |
+--------+--------+=====================+
hdr  = (mux << 12) | (len - 1)        len in 1..4096
```

| mux  | direction | meaning |
|------|-----------|---------|
| 0    | both      | command packet (C→S) / response packet (S→C) |
| 1    | S→C       | asynchronous notify (PROGRESS, OUTPUT) |
| 2–3  | —         | unused; client silently discards |
| 4–15 | both      | bulk-data FIFOs; wire mux = FIFO index + 4 |

* A mux-0 payload is at most 4096 bytes (client buffers are exactly 4096).
* FIFO data for one logical transfer may be split across many packets; the
  receiver concatenates per mux.
* The client never sends a payload of length 0 (len-1 encoding can't express it).

## 2. Blocks inside a mux-0 packet

A mux-0 packet is a sequence of blocks:

```
C→S command:  [cmd u8][0 u8][blocklen u16][args ...]
S→C response: [err u8][0 u8][blocklen u16][results ...]
```

`blocklen` includes the 4-byte header. `err` is an `AJI_ERROR` (low byte;
all wire values are < 128). Unread trailing bytes in a block are skipped, so a
server may append fields and a client may ignore them (this is how versions
extend messages).

Primitive encodings inside blocks:

| type   | encoding |
|--------|----------|
| int    | 4 bytes BE |
| long   | 8 bytes BE |
| string | `[n u8][n bytes]`, no NUL, n < 256 (NULL C-string sends n=0) |
| raw(n) | n bytes, length known from context |

## 3. Connection handshake

1. Client connects. **Server speaks first** with one mux-0 packet that is *not*
   block-framed (client reads it as a single raw block):
   ```
   string "JTAG Server\r\n"
   int    server_version          (≤ 0xFFFF; current = 13)
   int    authtype                 0 = none, 0xF0 = AUTHENTICATE_MD5
   raw16  challenge                only if authtype == 0xF0
   ```
2. If `authtype == 0xF0`: client sends `AUTHENTICATE_MD5 { raw16 md5(challenge ‖ password) }`;
   server replies one response block (`AJI_NO_ERROR` or failure → client marks AUTHFAIL).
3. If `server_version >= 2`, client sends **one packet with two commands**:
   `USE_PROTOCOL_VERSION { int v = min(server_version, 13) }`, `GET_VERSION_INFO {}`.
   Server replies **one packet with two response blocks**:
   ```
   resp(USE_PROTOCOL_VERSION): int server_flags        (bit0 SERVER_ALLOW_REMOTE)
   resp(GET_VERSION_INFO):     string version_info
                               int    pgmparts_version  (read, then ignored)
                               string server_path       (optional)
   ```
4. Connected. From here the client always sends a mux-0 packet and blocks for
   exactly one mux-0 packet back (plus any FIFO/notify traffic).

The client gives the server 10 s to send the greeting.

## 4. Request/response pairing and batching

* **One response packet per command packet.** It contains one response block
  per command block, in order.
* The client batches device-level commands (`AJI_PACK_MANUAL/AUTO/STREAM`):
  up to 64 commands per packet, up to 32 read FIFOs per packet, and <1024
  bytes of pending TX FIFO data.
* On the client, response parsing stops at the first non-zero `err`. So a
  server may stop executing at the first failing command and omit the remaining
  responses **[inferred: real jtagd most likely does exactly this; it must
  then also discard the TX FIFO bytes those skipped commands would have consumed]**.
* `CONTINUE_COMMANDS (0xFF)`: in streaming mode the client sends a packet,
  then starts the next packet with a `CONTINUE_COMMANDS` block (no args),
  *without* waiting for the previous response. It expects a response block for
  it like any other command. Pipelined packets therefore get pipelined
  responses, one per packet. **[inferred: semantics are "continue the
  previous batch"; if the previous packet failed the server most likely fails
  this one too.]**

## 5. FIFOs (bulk data)

Large/variable data never sits inside a block; it rides on mux 4..15.

* **C→S**: the command block arrives first, then the TX FIFO bytes. Each wire mux
  is an independent byte stream; a command consumes the next N bytes of the
  mux it uses, where N is derivable from the command's args. All commands in
  the current client use FIFO index 0 (mux 4), except `ANY_SEQUENCE`, which
  sends TDI on index 0 (mux 4) and TMS on index 1 (mux 5), interleaved.
  A TX FIFO with no source buffer sends zeros.
* **S→C**: the client queues receive FIFOs per mux in command order; incoming
  mux-4 bytes fill the head of the queue until it has its expected length,
  then move to the next. Data can arrive before or after the response packet
  (the client keeps pumping FIFO packets while waiting for mux 0). Simplest
  server rule: **send FIFO data for a command before the response packet that
  contains its response.**
* List-style replies (hardware, chain, device lists) use an unknown-length RX
  FIFO on mux 4; the response block carries `fifo_len`, and the FIFO payload
  is itself an un-framed sequence of primitive fields.

## 6. Notifications (mux 1, S→C, any time)

Block-framed, header byte = command:

| cmd | body | client action |
|-----|------|---------------|
| `PROGRESS 0x9E` | int data_used | progress callback on the open device currently filling a batch |
| `OUTPUT 0x9F`   | int level, then NUL-terminated text at block offset 8 | output callback |

Others are ignored. A mock never needs to send these, but sending them is a
cheap robustness test.

## 7. Commands

`→` = request args after the 4-byte header, `←` = response fields after the
4-byte header (present when `err == 0` unless stated). `v` = negotiated
protocol version. `chain_id` is the server's handle for a cable;
`open_id` is the server's handle for an opened device.

These are **not** the `AJI_CHAIN_ID` / `AJI_OPEN_ID` of the C API. Those are
pointers to client-side objects (`AJI_CHAIN_JS`, `AJI_OPEN_JS`), and each object
stores the wire id (`m_chain_id`, `m_open_id`) the server handed out. The
pointer never crosses the wire; the u32 never reaches the API caller (except
folded into `AJI_HARDWARE.persistent_id`).

### Server / hardware

| cmd | → | ← |
|-----|---|---|
| `GET_HARDWARE 0x80` | v≥1: int v | int n, int fifo_len; FIFO mux4: n × { int chain_id, str hw_name, str port, int chain_type, str device_name, int features (v≥2) } |
| `ADD_HARDWARE 0x81` | int hw_type, str port, (v≥1) str hw_name, str device_name | — |
| `REMOVE_HARDWARE 0xA1` | int chain_id | — |
| `GET_VERSION_INFO 0x83` | — | see §3 |
| `GET_DEFINED_DEVICES 0x84` | int tag | int tag, int n, int fifo_len; FIFO: n × { int idcode, int irlen, int features, int mask, int 0, str name } |
| `DEFINE_DEVICE 0xA7` | int chain_id, int tap_pos, int idcode, int irlen, int features, int mask, int 0, str name | — (chain_id=0,tap=0 = global define) |
| `GET_QUARTUS_DEVICES 0x89` | — | int n, int fifo_len; FIFO as GET_DEFINED_DEVICES |
| `CHECK_PASSWORD 0xF1` | int authtype, raw16 challenge, raw16 response | — |
| `USE_PROTOCOL_VERSION 0xFE` | int v | int server_flags |
| `PING 0xFD` | — | — (debug builds only) |

`AJI_HARDWARE.persistent_id` is built client-side as
`((chain_id & 0xFFFF) | (link_id << 24)) + 1`; a `chain_id` of 0 is reserved for
dummy entries, so **real chain ids must be non-zero**.

### Chain level (client must have done `LOCK_CHAIN`)

| cmd | → | ← |
|-----|---|---|
| `LOCK_CHAIN 0xA2` | int chain_id, int timeout_ms | — ; on `CHAIN_IN_USE` (36): blocker info |
| `UNLOCK_CHAIN 0xA3` | int chain_id | — |
| `SCAN_CHAIN 0xA4` / `READ_CHAIN 0xA5` | int chain_id, int known_tag, (v≥2) int autoscan | int chain_tag, int n, int fifo_len; FIFO: n × { int idcode, int irlen, int features, int 0, int 0, str name } |
| `SET_PARAMETER 0xAA` | int chain_id, str name, int value | — |
| `GET_PARAMETER 0xAB` | int chain_id, str name | int value |
| `GET_PARAMETER_BLOCK 0xAD` | int chain_id, str name, int max, int txlen, raw(txlen) | int len, raw(len) |
| `WATCH_DATA 0xBE` | int chain_id, int on, [int intrusive, int key] | — (then a stream; low priority) |
| `OPEN_DEVICE 0xA8` | int chain_id, int tap_pos, int claim_n, claims…, str app_name | int open_id ; on `CHAINS_CLAIMED`(43)/`CHAIN_IN_USE`(36): blocker info |
| `OPEN_ENTIRE_CHAIN 0xA9` | int chain_id, int style-1, str app_name | int open_id ; blocker info on failure |

READ/SCAN chain list semantics on the client: `n == 0` clears the TAP list,
`n > 0 && fifo_len == 0` means "unchanged since `known_tag`", otherwise the
list is replaced. `chain_tag == 0` means "not configured". Unscanned chain +
`READ_CHAIN` with autoscan=0 should return `AJI_CHAIN_NOT_CONFIGURED` (38);
the client then retries with `SCAN_CHAIN`.

Claim encoding in `OPEN_DEVICE`:
* v≥13: per claim `int type, int length, long value`
* v 2–12: per claim `int type, int value` (WEAK claims only if v≥10)
* v<2: `int 0`, then `claim_n × int value`

Note the client may prepend an implicit `OVERLAY_SHARED HUB_INFO(0)` claim
when USER0+USER1 are both claimed, so `claim_n` on the wire can exceed what
the caller passed.

Blocker info: `int k`, then k × { str app_name, int (ip_ver<<16 | addrlen), raw(addrlen) }.

### Device level (open_id, after `LOCK_DEVICE`; batchable)

| cmd | → | ← |
|-----|---|---|
| `LOCK_DEVICE 0xC1` | int open_id, int timeout_ms | — ; blocker info on `CHAIN_IN_USE` |
| `UNLOCK_DEVICE 0xC2` | int open_id | — |
| `UNLOCK_LOCK_DEVICE 0xD3` | int unlock_id, int lock_id | — (ids may be an open_id **or a chain_id**: this one opcode is used for open→open, chain→open and open→chain handover, so a server must keep chain ids and open ids in disjoint ranges **[inferred]**) |
| `CLOSE_DEVICE 0xC0` | int open_id | — |
| `ACCESS_IR 0xC5` | int open_id, int instruction, int flags (bit0 = want capture) | int captured_ir |
| `ACCESS_IR_FIFO 0xCF` | int open_id, int flags, int length_ir; TX FIFO ⌈len/8⌉ bytes | — ; RX FIFO ⌈len/8⌉ bytes if flags bit0 |
| `ACCESS_DR 0xC7` | int open_id, int length, int flags, int w_off, int w_len, int r_off, int r_len, (v≥5) int batch; TX FIFO ⌈w_len/8⌉×batch | (v≥11) int timestamp ; RX FIFO ⌈r_len/8⌉×batch |
| `ANY_SEQUENCE 0xC9` | int open_id, int num_tcks, int want_tdo; TX FIFO idx0 TDI ⌈n/8⌉, idx1 TMS ⌈n/8⌉ | — ; RX FIFO TDO ⌈n/8⌉ if want_tdo |
| `RUN_TEST_IDLE 0xCA` | int open_id, int clocks, (v≥12) int flags | — |
| `TEST_LOGIC_RESET 0xCB` | int open_id | — |
| `DELAY_MICROSECONDS 0xCE` | int open_id, int usec | — |
| `CONTINUE_COMMANDS 0xFF` | — | — (§4) |

Bit order in all bit buffers: LSB of byte 0 is the first bit shifted (nearest TDO).

## 8. Client behaviours worth knowing

Found by round-tripping the reference client against the mock:

* **Deferred results.** With `AJI_PACK_AUTO/MANUAL/STREAM`, claimed commands
  are batched and their outputs (`captured_ir`, DR read buffers) are only
  written at `aji_flush()` or the next non-deferred call.
* **`aji_unlock_chain_lock(..., AJI_PACK_STREAM)` never streams.** It stores
  the pack style verbatim, whereas `aji_lock()` turns STREAM into "MANUAL +
  pipelined `CONTINUE_COMMANDS`". Only locks taken via `aji_lock` stream.
* **Server-qualified hardware names.** With `ReplaceLocalJtagServer` (or any
  remote), cables belong to server `"host:port"`, so `aji_find_hardware` needs
  `"USB-Blaster on host:port [USB-1]"` (what `aji_print_hardware_name`
  prints), not `"USB-Blaster [USB-1]"`.
* **Errors stop the batch.** The client stops reading response blocks at the
  first error and leaves the device lock held, so the same open can carry on.
* **Upstream bugs fixed on this branch** (each crashed or tripped ASan in a
  round-trip test): 8-byte store into a 4-byte buffer for numeric-IP hosts
  (`TCPCLIENT::lookup`); `new[]`/`delete` mismatch on blocker info; freeing an
  interior pointer and keeping the caller's password pointer when a temporary
  remote server is added twice; null-pointer arithmetic in the config reader.

## 9. What a mock does *not* get to skip

The client does SLD hub/node discovery (`aji_get_nodes`) entirely in client
code by driving `ACCESS_IR(USER1)`, `ACCESS_DR(64-bit HUB_INFO overlay)`,
`ACCESS_IR(USER0)`, then reading 4-bit DR nibbles in batches. If the code
under test calls `aji_get_nodes`/`aji_open_node`, the mock must return a
plausible SLD hub nibble stream on USER0 after that sequence. Pure
"answer every DR with zeros" will make node discovery report no hub.

## 10. Validation

Every **[inferred]** item, and ideally the whole table, should be checked by
capturing traffic between `jtagconfig` (or `libaji_client`) and a real
Quartus jtagd. The framing is simple enough that a capture can be decoded
with a few dozen lines of Python and diffed against what the mock emits.
