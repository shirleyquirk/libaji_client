# mock-jtagd

A stand-in for the Quartus JTAG server (jtagd / jtagserver.exe) for testing
AJI clients without Quartus or hardware. It speaks the wire protocol in
[`jtagd-protocol.md`](jtagd-protocol.md) and models just enough JTAG to give
deterministic answers.

```
test/
  mock_jtagd/   library (+ mock-jtagd executable): protocol codec, server, chain model
  roundtrip/    gtest suite: the real libaji_client against the mock
```

## Build and test

Linux, gcc 14, CMake ≥ 3.25:

```sh
git submodule update --init
cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_C_COMPILER=gcc-14
cmake --build build -j
ctest --test-dir build -j
```

googletest and yaml-cpp are used from the system if they're installed, and fetched otherwise.
Each test case runs in its own process, because libaji_client keeps its connections
in process globals. Set `MOCK_JTAGD_TRACE=1` to log every command the server
sees.

## Scenario files

The hardware is described in YAML (examples in `test/scenarios/`). Why the
format looks like this, and what a passing test is evidence of, is in
[`mock-jtagd-design.md`](mock-jtagd-design.md).

```yaml
protocol_version: 13            # advertised; the client negotiates down. Default 13
password: hunter2               # optional: require MD5 auth
allow_remote: false
cables:
  - hw_name: USB-BlasterII
    port: "1-3"
    parameters: { JtagClock: 24000000 }
    chain:                      # nearest TDO first, i.e. jtagconfig order
      - name: 5CSEMA5
        idcode: 0x02D1_20DD     # decimal, 0x.. or 0b.., '_' allowed anywhere
        irlen: 10
        idcode_ir: 0x006        # default 0x006; selected by TLR
        ir_capture: 0b01        # default 1
        features: [bad_ir_capture]   # possible_hub is added automatically with a hub
        registers:              # TAP-level DRs by IR value; anything else is a free-length rw
          0x055: rw             # shorthand for { kind: rw }
          0x00F: { kind: const, length: 32, value: 0xCAFE_F00D }
        channels:               # command/response on private TAP instructions (same keys as node channels)
          - { name: priv, command_ir: 0x2B0, response_ir: 0x2B1, transcript: [] }
        hub:
          ir_bits: 8            # node IR width (USER1 = select bits + ir_bits)
          nodes:                # node select = position + 1
            - name: sdm         # used in error messages and --check
              idcode: { version: 1, node: 0x7A, mfg: 0x6E, instance: 0 }   # or 0x0BD06E00
              registers:        # by node IR value
                0x0: { kind: sequence, length: 8, values: [0, 0, 1], provenance: inferred }
              channels:
                - name: packets
                  command_ir: 0x1     # DR scans here append words to the request
                  response_ir: 0x2    # DR scans here read queued response words
                  word_bits: 32
                  idle: 0             # read when nothing is queued
                  provenance: placeholder
                  transcript:
                    - request:  [0x0000_0010]
                      response: [0x0000_1000, 0x02D1_20DD]
```

| Register kind | Capture returns |
|---------------|-----------------|
| `rw` (default) | what the previous scan wrote (initially `value`) |
| `const` | `value`, whatever was written |
| `sequence` | `values` in order, then the last one forever |

A channel (on a hub node, or on the TAP's own IRs as in `scenarios/sdm_tap.yaml`)
matches the accumulated request words against its transcript. A
request that can't become any entry is a **model error**: the round-trip
fixture fails the test on any model error left at teardown, so code can't
quietly talk to hardware the scenario doesn't describe.
`provenance` (`observed | documented | inferred | placeholder`) is optional
on devices, nodes, registers and channels, and is shown by `--check`.

The loader is strict. Unknown keys, out-of-range values, IR clashes and
nested hubs (not modelled yet) are errors with a position:

```
$ mock-jtagd --scenario bad.yaml --check
bad.yaml:7:25: unknown key 'nod' in node idcode (expected one of: version, node, mfg, instance)
```

`--check` prints the board as the client would see it:

```
$ mock-jtagd --scenario test/scenarios/sdm_hello.yaml --check
protocol v13
1) USB-BlasterII [1-3]
  4341A0DD   AGFB014R24A  irlen 10  [placeholder]
    Node 08406E00  virtual_jtag (mfg 0x6E node 0x08 v1 #0)
    Node 0BD06E00  sdm          (mfg 0x6E node 0x7A v1 #0)  [placeholder]
      IR 0x0: const 32 bits = 0x5D30001  [placeholder]
      channel packets: command IR 0x1, response IR 0x2, 32-bit words, 1 transcript entries  [placeholder]
```

## What it models

| Area | Behaviour |
|------|-----------|
| Protocol | versions 1–13 (advertised version is configurable; the client negotiates down), optional MD5 auth, batching, `CONTINUE_COMMANDS`, FIFOs in both directions |
| Locks | chain and device locks shared across connections, with timeouts, blocker info and handover via `UNLOCK_LOCK_DEVICE`; a disconnecting client drops its locks |
| Claims | exclusive vs shared vs weak conflicts between opens on the same TAP **[inferred rules]** |
| TAPs | configurable IR capture (default `'b..01`); IDCODE and BYPASS; declared `rw`/`const`/`sequence` registers; any other instruction is a free-length rw register. Shift semantics are exact for odd lengths and offsets; TLR selects IDCODE |
| SLD hub | USER1/USER0 drive a hub ROM (hub IDCODE + node IDCODEs) so `aji_get_nodes` works; node registers and command/response channels are addressable via `aji_open_node` / `aji_access_overlay`. Hierarchical hubs are not modelled yet |
| Faults | per-command error injection (optionally after N successes) and connection drops |
| Not modelled | TAP state machine (`ANY_SEQUENCE` returns TDI as TDO), `WATCH_DATA`, `GET_PARAMETER_BLOCK`, progress/output notifications, protocol version 0 |

## Using it from a test

With the round-trip fixture (`test/roundtrip/fixture.hpp`), the hello world in
`test/roundtrip/sdm_test.cpp` is the pattern to copy: a toy client written
against the plain AJI API (`toy_sdm_client.hpp`, standing in for your code)
runs against `scenarios/sdm_hello.yaml`:

```cpp
TEST_F(RoundTrip, SdmHelloWorldReadsIdcode)
{
    start("sdm_hello.yaml");
    toy::SdmClient sdm;
    ASSERT_EQ(AJI_NO_ERROR, sdm.find());   // aji_get_nodes, match node id
    ASSERT_EQ(AJI_NO_ERROR, sdm.open());   // aji_open_node + lock
    toy::Response r;
    ASSERT_EQ(AJI_NO_ERROR, sdm.call(toy::get_idcode, {}, r));  // command IR, response IR
    EXPECT_EQ(0x4341A0DDu, r.data.at(0));
}
```

Directly with the library:

```cpp
#include "mock_jtagd/server.hpp"

mock_jtagd::Scenario s = mock_jtagd::load_scenario("board.yaml");      // or Scenario::default_board()
s.cables[0].taps[1].hub->nodes.push_back(mock_jtagd::sld_node_idcode(1, 0x08, mock_jtagd::mfg_altera, 2));
mock_jtagd::Server server(s);                    // ephemeral port on 127.0.0.1

// Point the client at it before the first aji_* call:
//   QUARTUS_JTAG_CLIENT_CONFIG=<file containing>
//   ReplaceLocalJtagServer = "127.0.0.1:<server.port()>";
// Replacing the local server (rather than adding Remote1) also stops the client
// from trying to launch a real jtagd.

server.inject({.cmd = mock_jtagd::Cmd::access_dr, .error = mock_jtagd::Err::failure, .skip = 2});
// ... drive the client ...
for (const auto & r : server.commands()) { /* session, packet, cmd, result, executed */ }
EXPECT_TRUE(server.take_model_errors().empty());
```

`test/roundtrip/fixture.hpp` wraps the above, plus `RawClient` for opening a
second, hand-rolled connection (e.g. to hold a lock the client then collides
with).

## Capturing from real hardware: jtag-proxy

`jtag-proxy` sits between your code and a real jtagd, forwards everything
unmodified, and records what passed: one line per **logical** scan, with
batching undone, so the same operations log identically however the client
packed them. Why it's built this way is in
[`mock-jtagd-design.md`](mock-jtagd-design.md#capture-a-logical-scan-proxy-not-pcap).

```sh
# 1. on the machine with the board: jtagd running as usual on 1309
build/test/jtag-proxy capture --upstream 127.0.0.1:1309 \
    --scan-log scan.yaml --wire-log wire.txt [--names board.yaml]
#    -> jtag-proxy listening on 127.0.0.1:41235 -> 127.0.0.1:1309
#    -> client config: ReplaceLocalJtagServer = "127.0.0.1:41235";

# 2. run your code with a client config containing that line
QUARTUS_JTAG_CLIENT_CONFIG=proxy.conf ./your-tool ...

# 3. Ctrl-C the proxy, then turn one node's traffic into scenario YAML
build/test/jtag-proxy transcript --scan-log scan.yaml \
    --node 0x0BD06E00 --command-ir 0x1 --response-ir 0x2 [--strip-idle 0] > sdm.yaml
```

What comes out:

- **Scans** (echoed to stderr and written to `--scan-log` as a YAML list):
  ```
  s1 #57 USB-BlasterII [1-3] tap0 AGFB014R24A sdm (0BD06E00) IR 0x1  DR 32b w=00000010
  s1 #58 USB-BlasterII [1-3] tap0 AGFB014R24A sdm (0BD06E00) IR 0x2  DR 32b r=00001000
  ```
  The proxy learns each hub's node table by watching the client's own
  enumeration, so node scans carry the node IDCODE (and a name, with
  `--names`) without being told the board layout.
- **Wire log**: every packet and command with arguments, responses and FIFO
  bytes, e.g. `S>C p7 2 of 3 responses: 0 1`. This is what answers
  protocol questions such as how jtagd behaves after an error in a batch.
- **Transcript**: a `channels:` snippet with `provenance: observed`, ready to
  paste into a node (or a TAP) in a scenario file. Repeated identical
  exchanges collapse; a request that got different responses is flagged,
  since the mock replays the first. `--strip-idle` drops "nothing yet" words a
  polling client read before the real response.

Decoding never affects forwarding: if the decoder hits something it doesn't
understand, it reports it and stops decoding that connection, and traffic
keeps flowing.

## Standalone

```sh
build/test/mock-jtagd [--scenario board.yaml] --port 0 --port-file /tmp/port --trace [--protocol-version 11] [--password pw]
```

It serves the scenario (or a built-in demo board) until SIGINT/SIGTERM; command-line options override the file. The unmodified
`jtagquery` tool (upstream `jtagconfig.cpp`) works against it:

```
$ QUARTUS_JTAG_CLIENT_CONFIG=client.conf build/src/jtag/jtagquery
1) USB-Blaster on 127.0.0.1:43023 [USB-1]
  4BA00477   SOCVHPS
  02D120DD   5CSEMA5
```
