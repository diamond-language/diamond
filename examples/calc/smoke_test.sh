#!/usr/bin/env bash
# Replays testdata/session.txt through calc, interpreted and as a
# `diamond build` binary, and checks the transcript and exit codes. Set
# DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# The session ends with deliberate errors, so calc exits 1.
status=0; "$diamond" calc.di < testdata/session.txt > "$work/interpreted.txt" || status=$?
[[ "$status" == 1 ]]
diff -u testdata/session.expected "$work/interpreted.txt"

"$diamond" build calc.di -o "$work/calc" > "$work/build.log"
status=0; "$work/calc" < testdata/session.txt > "$work/binary.txt" || status=$?
[[ "$status" == 1 ]]
diff -u testdata/session.expected "$work/binary.txt"

# A clean session exits 0.
printf '1 + 1\n' | "$work/calc" | grep -qx '=> 2'
echo "calc smoke test passed"
