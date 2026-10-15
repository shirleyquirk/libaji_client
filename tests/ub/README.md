# UB demonstrations for the C API

Each test here is **expected to trip AddressSanitizer/UBSan**. A test passes
when the sanitizer report contains every `// EXPECT:` line in that test, in
order. That is, when the hazard it documents has actually been observed.
No JTAG hardware or jtagd is needed.

```
$ tests/ub/run.sh
== building libaji_client with g++-14 and sanitizers     (only if the in-tree build isn't sanitized)
PASS  01_stale_chain_id  (UB observed; report in tests/ub/out/01_stale_chain_id.log)
PASS  02_dangling_hardware_strings  (UB observed; report in tests/ub/out/02_dangling_hardware_strings.log)
PASS  03_dotted_ip_overflow  (UB observed; report in tests/ub/out/03_dotted_ip_overflow.log)
PASS  04_temp_server_delete_mismatch  (UB observed; report in tests/ub/out/04_temp_server_delete_mismatch.log)
== 4 demonstrated, 0 not demonstrated
```

| Test | What it shows | Kind |
|---|---|---|
| `01_stale_chain_id` | Using an `AJI_CHAIN_ID` after the library has freed the chain object is a heap use-after-free at the first virtual call (`jtag_client_aji.cpp:199`), not `AJI_INVALID_CHAIN_ID`. | API hazard; the wrapper must prevent it |
| `02_dangling_hardware_strings` | `AJI_HARDWARE::server` (like `hw_name`, `port`, `device_name`) points into the chain object and dies with it. | API hazard; the wrapper must copy |
| `03_dotted_ip_overflow` | A dotted IPv4 server address overflows a 4-byte heap buffer in `TCPCLIENT::lookup`. | library bug |
| `04_temp_server_delete_mismatch` | Removing a temporary remote server frees a `new[]` block with `delete`. | library bug |

How it works without hardware: `run.sh` sets
`QUARTUS_JTAG_CLIENT_NO_LOCAL_SERVER=1` and an empty
`QUARTUS_JTAG_CLIENT_CONFIG`. Each test then adds a *temporary* remote server
at `localhost:1`, where nothing listens. The library reports one dummy chain
("Unable to connect") for it, which is a real library-owned `AJI_CHAIN_JS`.
Removing that server frees the chain object, the same `delete` a rescan does
when a cable is unplugged.

Requirements: `gcc-14`/`g++-14` (override with `CC`/`CXX`), autotools,
libtool, and the safestringlib submodule (`git submodule update --init`).
`run.sh` builds the library in-tree, because `configure` checks a relative
path for safestringlib. `--rebuild` forces a clean sanitized build. Tests 01
and 02 run with `alloc_dealloc_mismatch=0` so that the bug from test 04
doesn't stop them before they reach their own UB.

Background and line references: [`docs/cpp-wrapper/FINDINGS.md`](../../docs/cpp-wrapper/FINDINGS.md) §2 and §4.
