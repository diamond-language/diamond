#!/usr/bin/env bash
# Self-hosting Phase 3 differential harness: for every
# tests/parser_cases/*.di file, compares the real C-compiled-and-run
# result against the same source run through selfhost/parser.di's
# Diamond-language Parser + the native ProgramBuilder bridge. The curated
# positive corpus grows with the supported grammar; parser_error_cases
# separately locks matching rejection diagnostics into both compilers.
# See docs/roadmap.md's self-hosting Phase 3 entry.
set -euo pipefail

diamond=./build/diamond

count=0
for case_file in tests/parser_cases/*.di; do
    expected="$("$diamond" "$case_file")"
    actual="$(echo "$case_file" | "$diamond" selfhost/parser_run.di | sed '$d')"
    if [[ "$actual" != "$expected" ]]; then
        echo "parser result mismatch for $case_file" >&2
        echo "  expected: $expected" >&2
        echo "  actual:   $actual" >&2
        exit 1
    fi
    count=$((count + 1))
done

echo "$count parser differential cases passed"

error_count=0
for case_file in tests/parser_error_cases/*.di; do
    expected_file="${case_file%.di}.err"
    expected="$(cat "$expected_file")"
    if "$diamond" "$case_file" >/tmp/diamond-parser-native.out 2>&1; then
        echo "native compiler accepted parser error case $case_file" >&2
        exit 1
    fi
    if ! grep -Fq "$expected" /tmp/diamond-parser-native.out; then
        echo "native compiler error mismatch for $case_file" >&2
        exit 1
    fi
    if echo "$case_file" | "$diamond" selfhost/parser_check.di >/tmp/diamond-parser-selfhost.out 2>&1; then
        echo "self-hosted parser accepted error case $case_file" >&2
        exit 1
    fi
    if ! grep -Fq "$expected" /tmp/diamond-parser-selfhost.out; then
        echo "self-hosted parser error mismatch for $case_file" >&2
        exit 1
    fi
    error_count=$((error_count + 1))
done

echo "$error_count parser error differential cases passed"

depth_dir="$(mktemp -d)"
trap 'rm -rf "$depth_dir"' EXIT
for depth in $(seq 0 128); do
    if [[ "$depth" -eq 128 ]]; then
        printf '42\n' >"$depth_dir/f$depth.di"
    else
        printf 'require "f%s"\n' "$((depth + 1))" >"$depth_dir/f$depth.di"
    fi
done
if "$diamond" "$depth_dir/f0.di" >/tmp/diamond-parser-native.out 2>&1; then
    echo "native require-depth limit unexpectedly succeeded" >&2
    exit 1
fi
grep -Fq "require nesting limit reached" /tmp/diamond-parser-native.out
if echo "$depth_dir/f0.di" | "$diamond" selfhost/parser_check.di \
        >/tmp/diamond-parser-selfhost.out 2>&1; then
    echo "self-hosted require-depth limit unexpectedly succeeded" >&2
    exit 1
fi
grep -Fq "require nesting limit reached" /tmp/diamond-parser-selfhost.out
rm -rf "$depth_dir"
trap - EXIT

echo "require-depth differential case passed"
