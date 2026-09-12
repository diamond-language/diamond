#!/usr/bin/env bash
# Runs Thread's own test cases (tests/cases/thread_*.di), Channel's own
# (tests/cases/channel_*.di -- including the two real two-Thread producer/
# consumer cases, the actual reason this feature needs TSan coverage at
# all: a brand-new mutex+condvar primitive, see docs/internal/concurrency-
# internals.md), plus a couple of representative Fiber cases (the shared
# machinery the thread_local fix in diamond_fiber_entering touches)
# through the current build under ThreadSanitizer -- see docs/threads.md
# and the "make tsan" Makefile target. Deliberately narrower than the full
# tests/cases corpus: TSan's overhead makes re-running the entire (largely
# single-threaded, already covered by test-sanitize's ASan/UBSan pass)
# suite under it needlessly slow for a target whose only job is proving
# the new concurrency-specific code paths are race-free.
#
# Reuses build/run_cases (the same shared-process batch runner
# test/test-sanitize use) against a temporary directory populated with
# symlinks to just the matching cases, so the .expected/.expected_error
# comparison logic below doesn't have to reimplement anything beyond what
# those two conventions need.
set -euo pipefail

diamond_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$diamond_root"

run_cases_abs="$(pwd)/build/run_cases"
if [[ ! -x "$run_cases_abs" ]]; then
    echo "build/run_cases not found -- run 'make tsan' first" >&2
    exit 1
fi

case_dir="$(mktemp -d)"

for pattern in "tests/cases/thread_*" "tests/cases/channel_*" \
        "tests/cases/legacy_0318.*" "tests/cases/legacy_0321.*"; do
    for f in $pattern; do
        [[ -e "$f" ]] || continue
        ln -s "$(pwd)/$f" "$case_dir/$(basename "$f")"
    done
done

output_dir="$(mktemp -d)"
trap 'rm -rf "$case_dir" "$output_dir"' EXIT

TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1}" "$run_cases_abs" "$case_dir" "$output_dir"

case_count=0
for case_file in "$case_dir"/*.di; do
    case_name="${case_file%.di}"
    case_base="$(basename "$case_name")"
    if [[ -f "$case_name.expected" ]]; then
        actual="$(<"$output_dir/$case_base.stdout")"
        exit_code="$(<"$output_dir/$case_base.exitcode")"
        expected="$(cat "$case_name.expected")"
        if [[ "$exit_code" != "0" ]]; then
            echo "FAIL: $case_base (exit $exit_code, expected 0)" >&2
            cat "$output_dir/$case_base.combined" >&2
            exit 1
        fi
        if [[ "$actual" != "$expected" ]]; then
            echo "FAIL: $case_base" >&2
            echo "  expected: $expected" >&2
            echo "  actual:   $actual" >&2
            exit 1
        fi
        case_count=$((case_count + 1))
    elif [[ -f "$case_name.expected_error" ]]; then
        actual="$(<"$output_dir/$case_base.combined")"
        pattern="$(cat "$case_name.expected_error")"
        if [[ "$actual" != *"$pattern"* ]]; then
            echo "FAIL: $case_base" >&2
            echo "  expected error containing: $pattern" >&2
            echo "  actual: $actual" >&2
            exit 1
        fi
        case_count=$((case_count + 1))
    fi
done

echo "test-tsan: ran $case_count case(s) under ThreadSanitizer, no races reported"
