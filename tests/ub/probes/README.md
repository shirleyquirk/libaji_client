# Probes

Exploration programs written while finding the hazards in `../`. They print
what the library does; they don't assert anything, and `run.sh` doesn't run
them. Each file's header says what was observed.

The first stale-id probe became `../01_stale_chain_id.cpp` and
`../02_dangling_hardware_strings.cpp`, so it isn't kept separately.

Build one against the sanitized library (after `../run.sh` has built it):

```
DEFS="-DLITTLE=1 -DBIG=2 -DENDIAN=LITTLE -DWINDOWS=1 -DUNIX=2 -DLINUX=3 -DPORT=UNIX -DSYS=LINUX -DMODE_64_BIT -D_GLIBCXX_USE_CXX11_ABI=0"
g++-14 -std=c++17 -g -fsanitize=address,undefined -fcf-protection=none $DEFS \
  -Isrc/h -Itests/ub tests/ub/probes/enumerate_dummy_chain.cpp \
  -Lsrc/jtag/.libs -laji_client -Wl,-rpath,$PWD/src/jtag/.libs -o /tmp/probe
ASAN_OPTIONS=detect_leaks=0 QUARTUS_JTAG_CLIENT_NO_LOCAL_SERVER=1 \
  QUARTUS_JTAG_CLIENT_CONFIG=/dev/null /tmp/probe
```
