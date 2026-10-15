#!/usr/bin/env bash
# Compile checks backing claims in docs/cpp-wrapper/DESIGN.md.
# Runs with every compiler found among $CXX, g++-14, clang++.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$(mktemp -d)"; trap 'rm -rf "$out"' EXIT
src="$here/session_passing.cpp"
fail=0

compilers=()
for c in ${CXX:-} g++-14 clang++; do
    command -v "$c" >/dev/null && [[ ! " ${compilers[*]} " =~ " $c " ]] && compilers+=("$c")
done

for cxx in "${compilers[@]}"; do
    echo "== $cxx"
    if "$cxx" -std=c++23 "$src" -o "$out/ok" && "$out/ok"; then
        echo "PASS  default build compiles, runs, handback ordering holds"
    else
        echo "FAIL  default build"; fail=1
    fi
    for bad in BAD_REF BAD_SESSREF BAD_RETURN BAD_CONST_OP; do
        if "$cxx" -std=c++23 -D"$bad" "$src" -o "$out/$bad" 2>"$out/$bad.err"; then
            echo "FAIL  -D$bad compiled but must not"; fail=1
        else
            echo "PASS  -D$bad rejected: $(grep -m1 -E '^[^ ].*: error: ' "$out/$bad.err" | sed 's/.*: error: //')"
        fi
    done
done
exit $fail
