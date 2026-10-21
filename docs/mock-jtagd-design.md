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

## Plan

1. YAML scenario loader, register kinds, transcript channels, `--check`.
   *(this change)*
2. Hello world: find the node by IDCODE fields, drive its virtual IR, run one
   command/response exchange (SDM-style GET_IDCODE, placeholder transcript).
   *(this change)*
3. `mock_jtagd::gtest` fixture library + CMake package for external projects;
   a toy wrapper as the worked example.
4. `jtagconfig` importer (needs real dumps).
5. Capture tooling: record USER0/USER1 exchanges against a real jtagd and emit
   transcript YAML. This closes the evidence loop for packet services.
6. Named-node log matchers; callbacks; JTAG-to-Avalon and nested hubs as needed.
