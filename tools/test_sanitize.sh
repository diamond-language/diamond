#!/usr/bin/env bash
# Runs the test suite under the sanitizers (what `make test-sanitize` runs after building them).
#
# A failure is retried once, because shared CI runners occasionally trip a timing assumption in the test harness itself:
# from 2026-09-05 to 2026-10-19 the first attempt failed in 4 of about 590 sanitizer jobs, and the two it masked were a 10 s
# timeout around a GC-stress client and a duplicated SIGINT from timeout(1), not anything the sanitizers found.
#
# The retry must never hide a real finding, though: a sanitizer report in attempt 1 fails the run at once, since a memory error
# that shows up on one run in two is exactly what a second, green attempt would otherwise bury. A retry that does happen is
# announced (a GitHub warning annotation, and a line starting "test-sanitize: attempt 1 failed" that scans of old logs can find)
# with the first failing line, so a flake that gets more frequent shows up instead of staying green.
#
# RUN_TESTS overrides the command that runs the suite, for tests/test_sanitize_test.sh.
set -uo pipefail
cd "$(dirname "$0")/.."
run_tests=${RUN_TESTS:-"bash tests/run.sh"}
log="$(mktemp)"
trap 'rm -f "$log"' EXIT

# $${VAR:-default}, not a bare VAR=value: a bare assignment would defeat a caller's own setting (CI sets ASAN_OPTIONS=detect_leaks=0,
# because LeakSanitizer's ptrace-based scan is unreliable in GitHub's containers). detect_leaks=1 applies only when the caller set nothing.
attempt() {
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1}" UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1}" \
        bash -c "$run_tests" 2>&1 | tee "$log"
    return "${PIPESTATUS[0]}"
}

if attempt; then exit 0; fi

if grep -Eq 'SUMMARY: (Address|UndefinedBehavior|Leak|Thread)Sanitizer|ERROR: (Address|Leak|Thread)Sanitizer|WARNING: ThreadSanitizer' "$log"; then
    echo "test-sanitize: attempt 1 reported a sanitizer finding; not retrying" >&2
    exit 1
fi

failure="$(grep -m1 'DIAGNOSTIC: failed at line' "$log" | cut -c1-200 || true)"
failure="${failure:-no DIAGNOSTIC line}"
echo "::warning title=test-sanitize retried::attempt 1 failed ($failure); retrying once" >&2
echo "test-sanitize: attempt 1 failed; retrying once ($failure)" >&2
attempt
