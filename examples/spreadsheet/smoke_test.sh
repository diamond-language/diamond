#!/usr/bin/env bash
# Runs spreadsheet through the interpreter and as a `diamond build` binary,
# and checks the grid, --cell, and every error path: a circular reference,
# division by zero, a malformed line, and a missing file.
# Set DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" spreadsheet.di testdata/budget.txt | cmp testdata/budget.expected -
"$diamond" spreadsheet.di testdata/budget.txt --cell C1 | grep -qx "1644.5"
"$diamond" spreadsheet.di testdata/budget.txt --cell D2 | grep -qx "889"

"$diamond" build spreadsheet.di -o "$work/spreadsheet" > "$work/build.log"
sheet="$work/spreadsheet"
"$sheet" testdata/budget.txt | cmp testdata/budget.expected -
"$sheet" testdata/budget.txt --cell C2 | grep -qx "411.125"

printf 'A1 = =B1\nB1 = =A1\n' > "$work/circular.txt"
status=0; "$sheet" "$work/circular.txt" 2> "$work/circular.err" > /dev/null || status=$?
[[ "$status" == 65 ]] && grep -q "circular reference: A1 -> B1 -> A1" "$work/circular.err"

printf 'A1 = 5\nA2 = 0\nA3 = =A1/A2\n' > "$work/divzero.txt"
status=0; "$sheet" "$work/divzero.txt" 2> "$work/divzero.err" > /dev/null || status=$?
[[ "$status" == 65 ]] && grep -q "division by zero" "$work/divzero.err"

printf 'A1 5\n' > "$work/badline.txt"
status=0; "$sheet" "$work/badline.txt" 2> "$work/badline.err" > /dev/null || status=$?
[[ "$status" == 65 ]] && grep -q "^line 1:" "$work/badline.err"

status=0; "$sheet" "$work/missing.txt" 2> "$work/missing.err" > /dev/null || status=$?
[[ "$status" == 66 ]] && grep -q "cannot open" "$work/missing.err"

status=0; "$sheet" 2> /dev/null || status=$?
[[ "$status" == 64 ]]

echo "spreadsheet smoke test passed"
