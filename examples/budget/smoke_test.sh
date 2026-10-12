#!/usr/bin/env bash
# Runs the budget host interpreted and as a `diamond build` binary and
# compares both with expected.txt. Each guest runs as a child process under
# DIAMOND_MAX_WALL_MILLISECONDS, so the host needs the diamond binary's path.
# Set DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" host.di "$diamond" > "$work/interpreted.txt"
diff -u expected.txt "$work/interpreted.txt"

"$diamond" build host.di -o "$work/budget" > "$work/build.log"
"$work/budget" "$diamond" > "$work/binary.txt"
diff -u expected.txt "$work/binary.txt"
echo "budget smoke test passed"
