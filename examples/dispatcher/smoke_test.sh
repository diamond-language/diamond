#!/usr/bin/env bash
# Runs the dispatcher demo interpreted and as a `diamond build` binary and
# compares both with expected.txt. Set DIAMOND_BIN to use a diamond other
# than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" dispatcher.di > "$work/interpreted.txt"
diff -u expected.txt "$work/interpreted.txt"

"$diamond" build dispatcher.di -o "$work/dispatcher" > "$work/build.log"
"$work/dispatcher" > "$work/binary.txt"
diff -u expected.txt "$work/binary.txt"
# Thread scheduling varies run to run; the output must not.
"$work/dispatcher" | diff -u expected.txt -
echo "dispatcher smoke test passed"
