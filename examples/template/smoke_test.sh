#!/usr/bin/env bash
# Runs template through the interpreter and as a `diamond build` binary,
# and checks the rendered invoice plus every error path: a mismatched
# closing tag, an unclosed section, an unmatched closing tag, a missing
# file, and bad JSON.
# Set DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" template.di testdata/invoice.mustache testdata/invoice.json | cmp testdata/invoice.expected -

"$diamond" build template.di -o "$work/template" > "$work/build.log"
template="$work/template"
"$template" testdata/invoice.mustache testdata/invoice.json | cmp testdata/invoice.expected -

printf '{}' > "$work/empty.json"

printf 'hi {{#a}}x{{/b}}' > "$work/mismatch.mustache"
status=0; "$template" "$work/mismatch.mustache" "$work/empty.json" 2> "$work/mismatch.err" > /dev/null || status=$?
[[ "$status" == 65 ]] && grep -q "expected {{/a}} but found {{/b}}" "$work/mismatch.err"

printf 'hi {{#a}}x' > "$work/unclosed.mustache"
status=0; "$template" "$work/unclosed.mustache" "$work/empty.json" 2> "$work/unclosed.err" > /dev/null || status=$?
[[ "$status" == 65 ]] && grep -q "unclosed section {{#a}}" "$work/unclosed.err"

printf '{{/close}}' > "$work/unmatched.mustache"
status=0; "$template" "$work/unmatched.mustache" "$work/empty.json" 2> "$work/unmatched.err" > /dev/null || status=$?
[[ "$status" == 65 ]] && grep -q "unmatched closing tag {{/close}}" "$work/unmatched.err"

status=0; "$template" "$work/missing.mustache" "$work/empty.json" 2> "$work/missing.err" > /dev/null || status=$?
[[ "$status" == 66 ]] && grep -q "cannot open" "$work/missing.err"

printf '{bad json' > "$work/bad.json"
status=0; "$template" "$work/unclosed.mustache" "$work/bad.json" 2> /dev/null > /dev/null || status=$?
[[ "$status" == 65 ]]

status=0; "$template" 2> /dev/null || status=$?
[[ "$status" == 64 ]]

echo "template smoke test passed"
