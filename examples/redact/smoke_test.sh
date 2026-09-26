#!/usr/bin/env bash
# Runs redact through the interpreter and as a `diamond build` binary, and
# checks tag mode, mask mode, the summary, stdin input, and exit codes.
# Set DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" redact.di --rules testdata/rules.txt testdata/app.log | cmp testdata/tagged.expected -

"$diamond" build redact.di -o "$work/redact" > "$work/build.log"
redact="$work/redact"
"$redact" --rules testdata/rules.txt testdata/app.log | cmp testdata/tagged.expected -
"$redact" --mask testdata/app.log | cmp testdata/masked.expected -
"$redact" --rules testdata/rules.txt < testdata/app.log | cmp testdata/tagged.expected -
"$redact" --summary --rules testdata/rules.txt testdata/app.log 2> "$work/summary.txt" > /dev/null
cmp testdata/summary.expected "$work/summary.txt"

# Tags stay consistent across files: the second file's first email is
# the same person as the first file's, so it keeps [email-1].
printf 'from ada.lovelace@example.com\n' > "$work/second.log"
"$redact" testdata/app.log "$work/second.log" | tail -1 | grep -qx 'from \[email-1\]'

status=0; "$redact" --rules testdata/bad_rules.txt testdata/app.log 2> "$work/bad.err" > /dev/null || status=$?
[[ "$status" == 65 ]] && grep -q "line 1:" "$work/bad.err"
status=0; "$redact" --rules 2> /dev/null || status=$?
[[ "$status" == 64 ]]
status=0; "$redact" --bogus 2> /dev/null || status=$?
[[ "$status" == 64 ]]
status=0; "$redact" "$work/missing.log" 2> "$work/missing.err" || status=$?
[[ "$status" == 66 ]] && grep -q "cannot open" "$work/missing.err"

echo "redact smoke test passed"
