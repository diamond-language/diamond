#!/usr/bin/env bash
# Runs the generators demo interpreted and as a `diamond build` binary and
# compares both with expected.txt. Set DIAMOND_BIN to use a diamond other
# than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" generators.d6 > "$work/interpreted.txt"
diff -u expected.txt "$work/interpreted.txt"

"$diamond" build generators.d6 -o "$work/generators" > "$work/build.log"
"$work/generators" > "$work/binary.txt"
diff -u expected.txt "$work/binary.txt"
echo "generators smoke test passed"
