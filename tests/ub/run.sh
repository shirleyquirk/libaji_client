#!/usr/bin/env bash
# Build libaji_client with AddressSanitizer + UBSan and run the UB
# demonstrations in this directory.
#
# Each test is *expected* to trip a sanitizer. A test PASSES when the
# sanitizer report contains every `// EXPECT:` line of the test, in order,
# i.e. when the UB it documents was actually observed.
#
# Usage:
#   tests/ub/run.sh            # build the library if needed, run all tests
#   tests/ub/run.sh --rebuild  # force bootstrap/configure/make with sanitizers
#   CXX=g++-14 CC=gcc-14 tests/ub/run.sh
#
# The library's configure only works in-tree (it checks a relative path for
# safestringlib), so the sanitized build replaces any existing in-tree build.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
CC="${CC:-gcc-14}"
CXX="${CXX:-g++-14}"
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
# The library's own flags add -mindirect-branch/-mfunction-return, which
# Ubuntu's gcc-14 rejects alongside its default -fcf-protection.
FLAGS="-g -O1 $SAN -fcf-protection=none"
# Platform macros the public headers need (mirrors src/jtag/Makefile.am).
DEFS="-DLITTLE=1 -DBIG=2 -DENDIAN=LITTLE -DWINDOWS=1 -DUNIX=2 -DLINUX=3 -DPORT=UNIX -DSYS=LINUX -DMODE_64_BIT -D_GLIBCXX_USE_CXX11_ABI=0"
libdir="$repo/src/jtag/.libs"
out="${UB_OUT:-$repo/tests/ub/out}"

is_sanitized() { [[ -f "$libdir/libaji_client.so" ]] && nm -D "$libdir/libaji_client.so" 2>/dev/null | grep __asan_ >/dev/null; }   # not grep -q: SIGPIPE + pipefail

if [[ "${1:-}" == "--rebuild" ]] || ! is_sanitized; then
    echo "== building libaji_client with $CXX and sanitizers"
    (
        cd "$repo"
        # bootstrap copies Makefile.am files into the safestringlib submodule;
        # re-run it if the submodule has been reset since.
        [[ -x configure && -f 3rdparties/safestringlib/Makefile.am ]] || ./bootstrap
        ./configure CC="$CC" CXX="$CXX" CFLAGS="$FLAGS" CXXFLAGS="$FLAGS" LDFLAGS="$SAN" >/dev/null 2>&1
        make clean >/dev/null 2>&1 || true
        make -j"$(nproc)" >/dev/null 2>&1
    )
fi

mkdir -p "$out"
export QUARTUS_JTAG_CLIENT_NO_LOCAL_SERVER=1
export QUARTUS_JTAG_CLIENT_CONFIG="$out/empty.conf"   # never written: tests only add temporary servers
: > "$QUARTUS_JTAG_CLIENT_CONFIG"

pass=0; fail=0
for src in "$here"/[0-9]*.cpp; do
    name="$(basename "$src" .cpp)"
    exe="$out/$name"
    "$CXX" -std=c++17 $FLAGS $DEFS -I"$repo/src/h" -I"$here" "$src" \
        -L"$libdir" -laji_client -Wl,-rpath,"$libdir" -o "$exe"

    extra="$(sed -n 's|^// ASAN_OPTIONS: ||p' "$src")"
    log="$out/$name.log"
    ASAN_OPTIONS="detect_leaks=0${extra:+:$extra}" UBSAN_OPTIONS=print_stacktrace=1 \
        timeout 60 "$exe" >"$log" 2>&1 || true

    # Every EXPECT line must appear, in order, each on a later line than the
    # previous match (so two patterns on one log line don't both match).
    ok=1; from=1
    while IFS= read -r want; do
        n="$(tail -n +"$from" "$log" | grep -nF -m1 -- "$want" | cut -d: -f1 || true)"
        if [[ -z "$n" ]]; then ok=0; echo "   missing: '$want'"; break; fi
        from=$((from + n))
    done < <(sed -n 's|^// EXPECT: ||p' "$src")

    if (( ok )); then
        echo "PASS  $name  (UB observed; report in ${log#$repo/})"
        pass=$((pass + 1))
    else
        echo "FAIL  $name  (expected sanitizer report not found; see ${log#$repo/})"
        fail=$((fail + 1))
    fi
done

echo "== $pass demonstrated, $fail not demonstrated"
(( fail == 0 ))
