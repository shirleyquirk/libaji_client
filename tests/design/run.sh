#!/usr/bin/env bash
# Compile checks backing claims in docs/cpp-wrapper/DESIGN.md.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
CXX="${CXX:-g++-14}"
out="$(mktemp -d)"; trap 'rm -rf "$out"' EXIT
src="$here/session_passing.cpp"
fail=0

"$CXX" -std=c++23 "$src" -o "$out/ok" && "$out/ok" && echo "PASS  default build compiles and runs" \
    || { echo "FAIL  default build"; fail=1; }

for bad in BAD1 BAD2; do
    if "$CXX" -std=c++23 -D"$bad" "$src" -o "$out/$bad" 2>"$out/$bad.err"; then
        echo "FAIL  -D$bad compiled but must not"; fail=1
    else
        echo "PASS  -D$bad rejected: $(grep -m1 -E '^[^ ].*: error: ' "$out/$bad.err" | sed 's/.*: error: //')"
    fi
done
exit $fail
