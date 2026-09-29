#!/usr/bin/env bash
# Runs huffman through the interpreter and as a `diamond build` binary,
# checks the compressed output is byte-for-byte reproducible and decodes
# back to the exact original, a single-symbol file, and every error path.
# Set DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" huffman.di --encode testdata/sample.txt "$work/sample.huf" > /dev/null
cmp testdata/sample.huf.expected "$work/sample.huf"
"$diamond" huffman.di --decode "$work/sample.huf" "$work/sample.out" > /dev/null
cmp testdata/sample.txt "$work/sample.out"

"$diamond" build huffman.di -o "$work/huffman" > "$work/build.log"
huffman="$work/huffman"
"$huffman" --encode testdata/sample.txt "$work/sample2.huf" > /dev/null
cmp testdata/sample.huf.expected "$work/sample2.huf"
"$huffman" --decode "$work/sample2.huf" "$work/sample2.out" > /dev/null
cmp testdata/sample.txt "$work/sample2.out"

printf 'aaaaaaaaaa' > "$work/single.txt"
"$huffman" --encode "$work/single.txt" "$work/single.huf" > /dev/null
"$huffman" --decode "$work/single.huf" "$work/single.out" > /dev/null
cmp "$work/single.txt" "$work/single.out"

printf '' > "$work/empty.txt"
status=0; "$huffman" --encode "$work/empty.txt" "$work/empty.huf" 2> "$work/empty.err" > /dev/null || status=$?
[[ "$status" == 65 ]] && grep -q "cannot encode an empty file" "$work/empty.err"

printf 'not a real container' > "$work/bad.huf"
status=0; "$huffman" --decode "$work/bad.huf" "$work/bad.out" 2> "$work/bad.err" > /dev/null || status=$?
[[ "$status" == 65 ]] && grep -q "bad magic" "$work/bad.err"

head -c 100 "$work/sample.huf" > "$work/truncated.huf"
status=0; "$huffman" --decode "$work/truncated.huf" "$work/trunc.out" 2> "$work/trunc.err" > /dev/null || status=$?
[[ "$status" == 65 ]] && grep -q "truncated data" "$work/trunc.err"

status=0; "$huffman" --decode "$work/missing.huf" "$work/x.out" 2> "$work/missing.err" > /dev/null || status=$?
[[ "$status" == 66 ]] && grep -q "cannot open" "$work/missing.err"

status=0; "$huffman" 2> /dev/null || status=$?
[[ "$status" == 64 ]]
status=0; "$huffman" --bogus a b 2> /dev/null || status=$?
[[ "$status" == 64 ]]

echo "huffman smoke test passed"
