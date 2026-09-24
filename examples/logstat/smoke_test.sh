#!/usr/bin/env bash
# Runs logstat through the interpreter and as a `diamond build` binary, and
# checks the report, stdin input, and exit codes. Set DIAMOND_BIN to use a
# diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" logstat.di testdata/sample.ndjson > "$work/interpreted.txt"
cmp testdata/sample.expected "$work/interpreted.txt"

"$diamond" build logstat.di -o "$work/logstat" > "$work/build.log"
"$work/logstat" testdata/sample.ndjson > "$work/binary.txt"
cmp testdata/sample.expected "$work/binary.txt"
"$work/logstat" < testdata/sample.ndjson | cmp testdata/sample.expected -
"$work/logstat" - < testdata/sample.ndjson | cmp testdata/sample.expected -

status=0; "$work/logstat" --strict testdata/sample.ndjson > /dev/null || status=$?
[[ "$status" == 1 ]]
status=0; "$work/logstat" --top 0 2> "$work/usage.err" || status=$?
[[ "$status" == 64 ]] && grep -q -- '--top needs a positive number' "$work/usage.err"
status=0; "$work/logstat" --bogus 2> /dev/null || status=$?
[[ "$status" == 64 ]]
status=0; "$work/logstat" "$work/missing.ndjson" 2> "$work/missing.err" || status=$?
[[ "$status" == 66 ]] && grep -q "cannot open" "$work/missing.err"

# Errors append to /dev/stderr rather than truncating it, so with both
# streams redirected to one file the message is still there.
status=0; "$work/logstat" testdata/sample.ndjson "$work/missing.ndjson" > "$work/both.txt" 2>&1 || status=$?
[[ "$status" == 66 ]] && grep -q "cannot open" "$work/both.txt"

"$work/logstat" --top 1 testdata/sample.ndjson | grep -q '^  \.\.\. 1 more$'
echo "logstat smoke test passed"
