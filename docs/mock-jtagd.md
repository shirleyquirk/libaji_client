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

googletest is used from the system if it's installed, and fetched otherwise.
Each test case runs in its own process, because libaji_client keeps its connections
in process globals. Set `MOCK_JTAGD_TRACE=1` to log every command the server
sees.

## What it models

| Area | Behaviour |
|------|-----------|
| Protocol | versions 1–13 (advertised version is configurable; the client negotiates down), optional MD5 auth, batching, `CONTINUE_COMMANDS`, FIFOs in both directions |
| Locks | chain and device locks shared across connections, with timeouts, blocker info and handover via `UNLOCK_LOCK_DEVICE`; a disconnecting client drops its locks |
| Claims | exclusive vs shared vs weak conflicts between opens on the same TAP **[inferred rules]** |
| TAPs | IR capture `'b..01`; IDCODE and BYPASS registers; every other instruction is a plain R/W data register (a scan returns what the previous scan wrote) with proper shift semantics for odd lengths and offsets; TLR selects IDCODE |
| SLD hub | USER1/USER0 drive a hub ROM (hub IDCODE + node IDCODEs) so `aji_get_nodes` works, and node registers are addressable via `aji_open_node` / `aji_access_overlay`. Hierarchical hubs are not modelled yet |
| Faults | per-command error injection (optionally after N successes) and connection drops |
| Not modelled | TAP state machine (`ANY_SEQUENCE` returns TDI as TDO), `WATCH_DATA`, `GET_PARAMETER_BLOCK`, progress/output notifications, protocol version 0 |

## Using it from a test

```cpp
#include "mock_jtagd/server.hpp"

mock_jtagd::Scenario s = mock_jtagd::Scenario::default_board(); // HPS DAP + FPGA with a 2-node hub
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
```

`test/roundtrip/fixture.hpp` wraps the above, plus `RawClient` for opening a
second, hand-rolled connection (e.g. to hold a lock the client then collides
with).

## Standalone

```sh
build/test/mock-jtagd --port 0 --port-file /tmp/port --trace [--protocol-version 11] [--password pw]
```

It serves `Scenario::default_board()` until SIGINT/SIGTERM. The unmodified
`jtagquery` tool (upstream `jtagconfig.cpp`) works against it:

```
$ QUARTUS_JTAG_CLIENT_CONFIG=client.conf build/src/jtag/jtagquery
1) USB-Blaster on 127.0.0.1:43023 [USB-1]
  4BA00477   SOCVHPS
  02D120DD   5CSEMA5
```
