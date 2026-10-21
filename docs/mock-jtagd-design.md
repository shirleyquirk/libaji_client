# mock-jtagd: design for testing your own AJI code

Goal: let a wrapper/driver built on libaji_client be tested in CI against a
described board, with no Quartus and no hardware. This records the reasoning
and the plan; `mock-jtagd.md` documents what exists today.

## What a passing test is evidence of

A mock of an undocumented protocol, written from the same understanding as the
code under test, only proves the two agree with each other. A passing test
is evidence about reality only to the extent that the mock's behaviour came
from somewhere other than the code's author. So the stack is split by what
is actually known:

| Layer | Source of truth | What a passing test shows |
|-------|-----------------|---------------------------|
| AJI wire protocol (jtagd) | reference client, round-trip tested in CI | real compatibility |
| JTAG TAP / SLD hub / virtual IR (USER1/USER0) | documented, and implemented by the reference client | real compatibility |
| Node payload protocols (SDM packet service, JTAG-to-Avalon, ...) | undocumented | only as good as the transcript behind it |

For the bottom layer the mock does **not** model the protocol: it replays a
**transcript** of request→response exchanges and fails the test on any
request it has no entry for. A model is a second implementation of a belief.
A transcript captured from hardware is evidence, and the same test goes from
self-consistent to evidence-backed when a placeholder transcript is replaced
by a captured one. Every register and channel in a scenario can carry
`provenance: observed | documented | inferred | placeholder`, and
`mock-jtagd --check` prints it, so it's visible how much weight a test carries.

Tests that hold whatever the payload semantics are worth writing even before
captures exist: framing and bit/word order through the virtual-IR plumbing,
behaviour on unexpected or short responses, error propagation through AJI,
locking, timeouts.

## Human interface

- **Scenario file (YAML)**: describes the hardware. It is the primary interface,
  checked in next to the tests. Strict: unknown keys are errors, and every
  error carries `file:line:col`.
- **C++ structs**: the same data model, for tweaks inside a test.
- **`jtagconfig` importer** (planned): paste `jtagconfig -n` output from a real
  board to get its topology.
- **`mock-jtagd --scenario board.yaml --check`**: validate and print the tree,
  jtagconfig-style, with provenance.

### Behaviour vocabulary

| Kind | Capture returns | Use |
|------|-----------------|-----|
| `rw` (default) | what the previous scan wrote | control/data registers |
| `const` | a fixed value | ID, version, status |
| `sequence` | the next value from a list each scan (the last repeats) | polling loops |
| channel | a command IR consumes words, a response IR yields the matched transcript response | packet services |
| callback (planned) | a named C++ function attached by the test | anything stateful |

Nested hubs are accepted by the schema design but rejected by the loader until
the server models bridges.

## Capture: a logical-scan proxy, not pcap

A packet capture loses the structure: the same logical operation looks
different depending on how the client batched it (IR+DR in one packet vs two
round trips, MANUAL vs NEVER packing, how FIFO data is chunked). So capture
happens above batching:

```
your code ── libaji ──▶ jtag-proxy ──▶ real jtagd ──▶ cable ──▶ board
                            │
                            ├─ wire log: every block and FIFO payload, both directions
                            └─ scan log: one line per logical IR/DR scan
```

`jtag-proxy` is a fake jtagd that forwards to the real one. It decodes both
directions with the mock's protocol code and tracks what the server tracks:
open id → TAP or node, the USER1 selection (node + virtual IR) per TAP, and
the current IR. Each DR scan is then logged by name, with its write and read
data:

```
sdm (tap 1, node 0x7A) IR 0x1: wrote 32b 00000010
sdm (tap 1, node 0x7A) IR 0x2: read  64b 4341A0DD_00001000
```

Batching is undone before logging, so equivalent client behaviour gives an
identical scan log. A second step turns scan logs into scenario YAML:
consecutive command-IR writes become a transcript request, the following
response-IR reads become its response, with `provenance: observed`. The
wire log is kept for the protocol-level open questions. Blind spot:
`ANY_SEQUENCE` carries raw TMS/TDI, which can't be mapped to logical scans
without simulating the TAP state machine, so it is logged raw only.

## Validation checklist

What would move each layer from "self-consistent" to "evidence":

1. **Dual-target tests.** Let the round-trip fixture point at a real jtagd
   (`MOCK_JTAGD_TARGET=host:port` plus a board scenario) and run the existing
   hardware/device/hub tests on a real board. Every test that passes on both
   is direct evidence the mock matches jtagd at the AJI level. Cheapest, and
   the highest value.
2. **jtagconfig dumps → scenario importer**, then `mock-jtagd --check` and
   real `jtagconfig -n` should print the same topology for the same board.
3. **jtag-proxy wire log** settles the protocol unknowns, each provoked on purpose:
   - batch error behaviour: does jtagd stop at the first failing command,
     and what does it answer a following `CONTINUE_COMMANDS` with;
   - claim conflict rules (exclusive vs shared vs weak, overlays);
   - the error for an unknown `GET_PARAMETER` name; behaviour after a bad password;
   - whether chain ids and open ids really live in disjoint ranges.
4. **JTAG-to-Avalon master**: its packet protocol is public, so it can be a
   real model rather than a transcript. A memory-pattern write/read-back test
   that passes against both the mock and a board with on-chip RAM validates
   the model and your J2A code together.
5. **SDM packet service**: capture a real exchange with jtag-proxy, replace
   the placeholder transcript in `sdm_hello.yaml` / `sdm_tap.yaml`, and mark
   it `observed`.

## Plan

1. YAML scenario loader, register kinds, transcript channels (on hub nodes
   and on TAP IRs), `--check`. *(done)*
2. Hello world: find the node by IDCODE fields, drive its virtual IR, run one
   command/response exchange (SDM-style GET_IDCODE, placeholder transcript).
   *(done)*
3. Dual-target fixture (checklist 1).
4. jtag-proxy: wire log + scan log, then the scan-log → YAML converter.
5. `jtagconfig` importer (needs real dumps).
6. `mock_jtagd::gtest` fixture library + CMake package for external projects.
7. JTAG-to-Avalon master model.
8. Named-node log matchers; callbacks; nested hubs as needed.
