#!/usr/bin/env bash
# Checks the agenda, the month calendar, and a shifted offset against
# expected output (interpreted and as a `diamond build` binary), plus a few
# calendar edge cases and the error exits. Set DIAMOND_BIN to use a diamond
# other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" agenda.di --today 2026-09-25 --days 10 testdata/events.txt | diff -u testdata/agenda.expected -
"$diamond" build agenda.di -o "$work/agenda" > "$work/build.log"
agenda() { "$work/agenda" "$@"; }
agenda --today 2026-09-25 --days 10 testdata/events.txt | diff -u testdata/agenda.expected -
agenda --today 2026-09-25 --cal testdata/events.txt | diff -u testdata/calendar.expected -
agenda --today 2026-09-28 --days 2 --offset -10:00 testdata/events.txt | diff -u testdata/offset.expected -

# "monthly 31" lands on the last day of shorter months, including February.
printf 'monthly 31 Month end\n' > "$work/month_end.txt"
agenda --today 2028-02-01 --days 31 "$work/month_end.txt" | grep -q "^Tue 29 Feb 2028"
agenda --today 2027-02-01 --days 30 "$work/month_end.txt" | grep -q "^Sun 28 Feb 2027"
# "last fri" in a month with five Fridays picks the fifth.
printf 'last fri Last Friday\n' > "$work/last.txt"
[[ "$(agenda --today 2026-10-01 --days 31 "$work/last.txt" | head -1)" == "Fri 30 Oct 2026" ]]
# Every 2 weeks from a Monday: the next two dates are 14 days apart.
printf 'every 2 weeks from 2026-09-07 Review\n' > "$work/biweekly.txt"
[[ "$(agenda --today 2026-09-08 --days 28 "$work/biweekly.txt" | grep -c 2026)" == 2 ]]
agenda --today 2026-09-08 --days 28 "$work/biweekly.txt" | grep -q "^Mon 21 Sep 2026"

status=0; agenda --today 2026-02-30 testdata/events.txt 2> "$work/err" || status=$?
[[ "$status" == 64 ]] && grep -q "no such date" "$work/err"
printf 'every funday 09:00 Nope\n' > "$work/bad.txt"
status=0; agenda "$work/bad.txt" 2> "$work/err" || status=$?
[[ "$status" == 65 ]] && grep -q "line 1: unknown weekday 'funday'" "$work/err"
status=0; agenda "$work/missing.txt" 2> /dev/null || status=$?; [[ "$status" == 66 ]]
status=0; agenda 2> /dev/null || status=$?; [[ "$status" == 64 ]]
echo "agenda smoke test passed"
