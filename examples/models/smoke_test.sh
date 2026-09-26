#!/usr/bin/env bash
# Runs the models demo interpreted and as a `diamond build` binary against
# expected.txt, then checks that a different schema changes what gets
# generated. Set DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" models.di testdata/schema.json | diff -u testdata/expected.txt -
"$diamond" build models.di -o "$work/models" > "$work/build.log"
"$work/models" testdata/schema.json | diff -u testdata/expected.txt -

# Methods come from the schema: drop Book's in_print field and in_print?
# no longer exists.
sed 's/"in_print": {"type": "Bool"}//; s/"isbn": {"type": "String", "required": true},/"isbn": {"type": "String", "required": true}/' \
  testdata/schema.json > "$work/schema.json"
status=0; "$work/models" "$work/schema.json" > "$work/out.txt" 2>&1 || status=$?
grep -q "Book responds to in_print?: false" "$work/out.txt"
status=0; "$work/models" 2> /dev/null || status=$?; [[ "$status" == 64 ]]
echo "models smoke test passed"
