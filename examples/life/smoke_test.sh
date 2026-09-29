#!/usr/bin/env bash
# Runs life through the interpreter and as a `diamond build` binary, and
# checks a glider over 4 generations plus every error path.
# Set DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" life.di testdata/glider.txt 4 | cmp testdata/glider.expected -

"$diamond" build life.di -o "$work/life" > "$work/build.log"
life="$work/life"
"$life" testdata/glider.txt 4 | cmp testdata/glider.expected -

printf '.#\n#\n' > "$work/uneven.txt"
status=0; "$life" "$work/uneven.txt" 1 2> "$work/uneven.err" > /dev/null || status=$?
[[ "$status" == 65 ]] && grep -q "every row must be the same width" "$work/uneven.err"

printf '.X\n#.\n' > "$work/badchar.txt"
status=0; "$life" "$work/badchar.txt" 1 2> "$work/badchar.err" > /dev/null || status=$?
[[ "$status" == 65 ]] && grep -q "unexpected 'X'" "$work/badchar.err"

status=0; "$life" "$work/missing.txt" 1 2> "$work/missing.err" > /dev/null || status=$?
[[ "$status" == 66 ]] && grep -q "cannot open" "$work/missing.err"

status=0; "$life" testdata/glider.txt abc 2> /dev/null || status=$?
[[ "$status" == 64 ]]
status=0; "$life" 2> /dev/null || status=$?
[[ "$status" == 64 ]]

echo "life smoke test passed"
