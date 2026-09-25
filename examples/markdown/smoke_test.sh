#!/usr/bin/env bash
# Converts testdata/sample.md interpreted and as a `diamond build` binary,
# and checks the HTML, stdin input, --stats, and exit codes. Set
# DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" markdown.di --toc testdata/sample.md > "$work/interpreted.html"
diff -u testdata/sample.html "$work/interpreted.html"

"$diamond" build markdown.di -o "$work/markdown" > "$work/build.log"
"$work/markdown" --toc testdata/sample.md | diff -u testdata/sample.html -
"$work/markdown" --toc < testdata/sample.md | diff -u testdata/sample.html -
"$work/markdown" --stats testdata/sample.md 2>&1 >/dev/null | diff -u testdata/sample.stats -

status=0; "$work/markdown" --bogus 2> /dev/null || status=$?
[[ "$status" == 64 ]]
status=0; "$work/markdown" "$work/missing.md" 2> "$work/missing.err" || status=$?
[[ "$status" == 66 ]] && grep -q "cannot open" "$work/missing.err"
echo "markdown smoke test passed"
