#!/usr/bin/env bash
# Runs udiff through the interpreter and as a `diamond build` binary:
# diffs at several context sizes, the ignore flags, exit codes, patch
# apply / reverse / rejection, and (when diff(1) is installed) a check
# that the output matches GNU diff byte for byte.
# Set DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" build udiff.di -o "$work/udiff" > "$work/build.log"

# Everything below runs against the interpreter first, then the binary.
for udiff in "$diamond udiff.di" "$work/udiff"; do
    # Exit status 1 means "they differ", so run, check the status, then compare.
    differs() { # differs EXPECTED_FILE udiff-args...
        local expected="$1"; shift
        local status=0
        $udiff "$@" > "$work/out" || status=$?
        [[ "$status" == 1 ]] && cmp "$expected" "$work/out"
    }
    differs testdata/diff.expected testdata/old.txt testdata/new.txt
    differs testdata/diff_u0.expected -U 0 testdata/old.txt testdata/new.txt
    differs testdata/diff_u1.expected -U 1 testdata/old.txt testdata/new.txt
    differs testdata/from_empty.expected testdata/empty.txt testdata/new.txt
    differs testdata/to_empty.expected testdata/old.txt testdata/empty.txt

    # Same file: no output, status 0. -q and --stat.
    status=0; $udiff testdata/old.txt testdata/old.txt > "$work/out" || status=$?
    [[ "$status" == 0 && ! -s "$work/out" ]]
    [[ "$($udiff -q testdata/old.txt testdata/new.txt || true)" == "Files testdata/old.txt and testdata/new.txt differ" ]]
    [[ "$($udiff --stat testdata/old.txt testdata/new.txt || true)" == "1 hunk, 4 insertions(+), 1 deletion(-)" ]]

    # -i and -w compare on a normalized form but still print the real text.
    status=0; $udiff testdata/old.txt testdata/shouty.txt > /dev/null || status=$?
    [[ "$status" == 1 ]]
    $udiff -i -w testdata/old.txt testdata/shouty.txt > "$work/out"
    [[ ! -s "$work/out" ]]

    # A patch applies forwards and backwards, to stdout or to -o.
    $udiff --apply testdata/diff.expected testdata/old.txt | cmp testdata/new.txt -
    $udiff --apply -R testdata/diff.expected testdata/new.txt | cmp testdata/old.txt -
    $udiff --apply -o "$work/patched.txt" testdata/diff_u0.expected testdata/old.txt
    cmp testdata/new.txt "$work/patched.txt"
    $udiff --apply testdata/from_empty.expected testdata/empty.txt | cmp testdata/new.txt -
    $udiff --apply testdata/to_empty.expected testdata/old.txt | cmp testdata/empty.txt -

    # Headers with timestamps, a/ b/ prefixes, and blank context lines that
    # lost their leading space all still parse.
    { printf -- '--- a/old.txt\t2026-09-26 10:00:00\n+++ b/new.txt\t2026-09-26 10:05:00\n'
      tail -n +3 testdata/diff.expected | sed 's/^ $//'; } > "$work/foreign.patch"
    $udiff --apply "$work/foreign.patch" testdata/old.txt | cmp testdata/new.txt -

    # A patch that doesn't fit is refused, names the line, and writes nothing.
    status=0; $udiff --apply testdata/diff.expected testdata/new.txt -o "$work/refused.txt" 2> "$work/err" || status=$?
    [[ "$status" == 2 && ! -e "$work/refused.txt" ]] && grep -q "does not match at line" "$work/err"
    printf 'nothing to see here\n' > "$work/empty.patch"
    status=0; $udiff --apply "$work/empty.patch" testdata/old.txt 2> "$work/err" || status=$?
    [[ "$status" == 2 ]] && grep -q "no hunks found" "$work/err"
    printf '@@ -1,2 +1,1 @@\n-a\n' > "$work/short.patch"
    status=0; $udiff --apply "$work/short.patch" testdata/old.txt 2> "$work/err" || status=$?
    [[ "$status" == 2 ]] && grep -q "hunk 1 says" "$work/err"

    # Usage and file errors.
    status=0; $udiff 2> /dev/null || status=$?
    [[ "$status" == 2 ]]
    status=0; $udiff --bogus testdata/old.txt testdata/new.txt 2> "$work/err" || status=$?
    [[ "$status" == 2 ]] && grep -q "unknown option --bogus" "$work/err"
    status=0; $udiff -U x testdata/old.txt testdata/new.txt 2> "$work/err" || status=$?
    [[ "$status" == 2 ]] && grep -q "needs a number" "$work/err"
    status=0; $udiff testdata/old.txt "$work/missing.txt" 2> "$work/err" || status=$?
    [[ "$status" == 2 ]] && grep -q "cannot open" "$work/err"
done

# Against diff(1): the same hunks and headers, on every context size.
if command -v diff > /dev/null && diff --version 2> /dev/null | head -1 | grep -qi "gnu\|diffutils"; then
    for context in 0 1 3 8; do
        diff -U "$context" testdata/old.txt testdata/new.txt | tail -n +3 > "$work/gnu.body" || true
        $diamond udiff.di -U "$context" testdata/old.txt testdata/new.txt > "$work/mine" || true
        tail -n +3 "$work/mine" | cmp "$work/gnu.body" -
    done
fi

echo "udiff smoke test passed"
