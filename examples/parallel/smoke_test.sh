#!/usr/bin/env bash
# Runs the parallel demo interpreted and as a `diamond build` binary and
# compares both with expected.txt. Set DIAMOND_BIN to use a diamond other
# than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" parallel.di > "$work/interpreted.txt"
diff -u expected.txt "$work/interpreted.txt"

"$diamond" build parallel.di -o "$work/parallel" > "$work/build.log"
"$work/parallel" > "$work/binary.txt"
diff -u expected.txt "$work/binary.txt"
# Thread scheduling varies run to run; the output must not.
"$work/parallel" | diff -u expected.txt -
# --time reports on stderr only, so stdout is unchanged.
"$work/parallel" --time 2> "$work/time.txt" | diff -u expected.txt -
grep -q "same answer serially: true" "$work/time.txt"
echo "parallel smoke test passed"
