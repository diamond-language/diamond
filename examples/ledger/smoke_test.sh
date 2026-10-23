#!/usr/bin/env bash
# Runs the ledger demo interpreted and as a `diamond build` binary and
# compares both with expected.txt. Set DIAMOND_BIN to use a diamond other
# than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" ledger.d6 > "$work/interpreted.txt"
diff -u expected.txt "$work/interpreted.txt"

"$diamond" build ledger.d6 -o "$work/ledger" > "$work/build.log"
"$work/ledger" > "$work/binary.txt"
diff -u expected.txt "$work/binary.txt"
echo "ledger smoke test passed"
