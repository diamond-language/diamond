#!/usr/bin/env bash
# Runs leaderboard through the interpreter and as a `diamond build` binary
# and checks the full event replay (rank-change announcements, ranking,
# statistics, --versus), the --quiet form, and every error path.
# Set DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" build leaderboard.di -o "$work/leaderboard" > "$work/build.log"

status_of() { local s=0; "$@" > /dev/null 2>&1 || s=$?; echo "$s"; }

for lb in "$diamond leaderboard.di" "$work/leaderboard"; do
  $lb testdata/season1.txt --top 4 --versus testdata/season2.txt | cmp testdata/full.expected -
  $lb testdata/season2.txt --quiet | cmp testdata/quiet.expected -
  # --versus works from the other side too.
  $lb testdata/season2.txt --quiet --versus testdata/season1.txt | grep -qx "season2.txt is outscored by season1.txt"

  [[ "$($lb testdata/bad.txt 2>&1 >/dev/null || true)" == "testdata/bad.txt:2: expected \`name score\`, got \`bob abc\`" ]]
  [[ "$(status_of $lb testdata/bad.txt)" == 65 ]]
  [[ "$(status_of $lb testdata/missing.txt)" == 66 ]]
  [[ "$(status_of $lb)" == 64 ]]
  [[ "$(status_of $lb testdata/season1.txt --top 0)" == 64 ]]
  [[ "$(status_of $lb testdata/season1.txt --top)" == 64 ]]
  [[ "$(status_of $lb testdata/season1.txt --versus)" == 64 ]]
done

echo "leaderboard smoke test passed"
