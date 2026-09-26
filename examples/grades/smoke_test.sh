#!/usr/bin/env bash
# Checks the grade report (interpreted and as a `diamond build` binary),
# the exit codes, and that every program in rejected/ fails to compile with
# the expected message. Set DIAMOND_BIN to use a diamond other than
# ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# The sample has rejected lines, so the report exits 1.
status=0; "$diamond" grades.di testdata/scores.csv > "$work/interpreted.txt" || status=$?
[[ "$status" == 1 ]]
diff -u testdata/scores.expected "$work/interpreted.txt"

"$diamond" build grades.di -o "$work/grades" > "$work/build.log"
status=0; "$work/grades" testdata/scores.csv > "$work/binary.txt" || status=$?
[[ "$status" == 1 ]]
diff -u testdata/scores.expected "$work/binary.txt"

grep -v -e ',physics,$' -e 'one hundred' -e 'extra' -e ',103' testdata/scores.csv > "$work/clean.csv"
"$work/grades" "$work/clean.csv" | grep -q '^13 scores from 5 students'
status=0; "$work/grades" 2> /dev/null || status=$?
[[ "$status" == 64 ]]
status=0; "$work/grades" "$work/missing.csv" 2> /dev/null || status=$?
[[ "$status" == 66 ]]

# Each rejected program must fail to compile (exit 65) with its message.
expect_rejected() {
  local status=0
  DIAMOND_NO_CACHE=1 "$diamond" "rejected/$1" > /dev/null 2> "$work/rejected.err" || status=$?
  [[ "$status" == 65 ]] || { echo "rejected/$1 exited $status, not 65" >&2; exit 1; }
  grep -qF "$2" "$work/rejected.err" || { echo "rejected/$1: wrong error" >&2; cat "$work/rejected.err" >&2; exit 1; }
}
expect_rejected wrong_argument.di "argument 'average' of letter: expected Float, got String"
expect_rejected missing_case.di "case is not exhaustive over its subject's known closed type; missing: Rejected"
expect_rejected nullable_result.di "expected Score, got Score | Nil"
expect_rejected wrong_return.di "expected String, got Int"
echo "grades smoke test passed"
