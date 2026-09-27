#!/usr/bin/env bash
# Runs pngmeta through the interpreter and as a `diamond build` binary:
# info on good and damaged files, strip and set round trips (checked by
# reading the result back), and exit codes. Set DIAMOND_BIN to use a
# diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" pngmeta.di info testdata/photo.png testdata/icon.png | cmp testdata/info.expected -

"$diamond" build pngmeta.di -o "$work/pngmeta" > "$work/build.log"
pngmeta="$work/pngmeta"
"$pngmeta" info testdata/photo.png testdata/icon.png | cmp testdata/info.expected -

# A chunk whose CRC doesn't match is reported, and the exit status is 1.
status=0; "$pngmeta" info testdata/corrupt.png > "$work/corrupt.txt" || status=$?
[[ "$status" == 1 ]] && cmp testdata/corrupt.expected "$work/corrupt.txt"

# strip keeps the image and drops every text/time chunk.
"$pngmeta" strip testdata/photo.png "$work/stripped.png" > /dev/null
"$pngmeta" info "$work/stripped.png" > "$work/stripped.txt"
! grep -q "tEXt\|tIME" "$work/stripped.txt"
grep -q "4x3, 8-bit RGB" "$work/stripped.txt"
grep -q "IDAT" "$work/stripped.txt"

# set replaces an existing keyword and adds a new one; every CRC written
# must check out when read back.
"$pngmeta" set testdata/photo.png "$work/a.png" Author "Grace Hopper" > /dev/null
"$pngmeta" set "$work/a.png" "$work/b.png" Title "Pier at dusk" > /dev/null
"$pngmeta" info "$work/b.png" > "$work/b.txt"
grep -q "Author: Grace Hopper" "$work/b.txt"
grep -q "Title: Pier at dusk" "$work/b.txt"
[[ "$(grep -c "Author:" "$work/b.txt")" == 1 ]]
! grep -q "CRC MISMATCH" "$work/b.txt"

status=0; "$pngmeta" strip testdata/corrupt.png "$work/x.png" 2> "$work/err.txt" || status=$?
[[ "$status" == 1 ]] && grep -q "fails its CRC check" "$work/err.txt"
status=0; "$pngmeta" info testdata/truncated.png 2> "$work/err.txt" || status=$?
[[ "$status" == 1 ]] && grep -q "truncated" "$work/err.txt"
status=0; "$pngmeta" info testdata/notpng.png 2> "$work/err.txt" || status=$?
[[ "$status" == 1 ]] && grep -q "not a PNG" "$work/err.txt"
status=0; "$pngmeta" set testdata/photo.png "$work/y.png" "" value 2> /dev/null || status=$?
[[ "$status" == 1 ]]
status=0; "$pngmeta" info "$work/missing.png" 2> "$work/err.txt" || status=$?
[[ "$status" == 66 ]] && grep -q "cannot open" "$work/err.txt"
status=0; "$pngmeta" 2> /dev/null || status=$?
[[ "$status" == 64 ]]

echo "pngmeta smoke test passed"
