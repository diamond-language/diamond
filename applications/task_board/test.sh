#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
diamond="${DIAMOND_BIN:-$root/build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
bash "$root/tools/install_local_cuts.sh" "$work" >/dev/null
cd "$work"
"$diamond" "$root/applications/task_board/store_test.d6"
run() { "$diamond" "$root/applications/task_board/app.d6" --db "$work/board.db" "$@"; }
[[ "$(run project Diamond)" == 'project #1: Diamond' ]]
[[ "$(run project Other)" == 'project #2: Other' ]]
[[ "$(run add 1 'Ship a board')" == 'task #1 [todo]: Ship a board' ]]
run add 2 'Other task' >/dev/null
[[ "$(run list --project 1 --status todo)" == '#1 [todo] Diamond / Ship a board' ]]
[[ "$(run edit 1 'Terminal board')" == 'task #1 [todo]: Terminal board' ]]
[[ "$(run move 1 doing)" == 'task #1 [doing]: Terminal board' ]]
[[ "$(run list --status doing --project 1)" == '#1 [doing] Diamond / Terminal board' ]]
[[ "$(run list --project 1 --status done)" == 'No tasks.' ]]
[[ "$(run projects)" == $'#1 Diamond\n#2 Other' ]]
reject() {
  local expected="$1" status=0
  shift
  run "$@" >"$work/out" 2>"$work/err" || status=$?
  [[ "$status" == "$expected" ]]
  [[ -s "$work/err" ]]
  [[ ! -s "$work/out" ]]
}
reject 65 move 1 unknown
reject 65 edit 1 ''
reject 65 project Diamond
reject 65 add 999 Orphan
reject 66 edit 999 Missing
reject 64 add 1oops Nope
reject 64 list --project
reject 64 list --status unknown
reject 64 list --status todo --status done
reject 64 nonsense
# Each command opens a fresh process. Rejected edits preserve stored values.
[[ "$(run list --project 1)" == '#1 [doing] Diamond / Terminal board' ]]
[[ "$(run move 1 done)" == 'task #1 [done]: Terminal board' ]]
[[ "$(run list --status doing)" == 'No tasks.' ]]
[[ "$(run list --project 1)" == '#1 [done] Diamond / Terminal board' ]]
BOARD_DATABASE_PATH="$work/unused.db" run projects >"$work/projects"
[[ ! -e "$work/unused.db" ]]
grep -q '#1 Diamond' "$work/projects"
"$diamond" "$root/applications/task_board/app.d6" --db "$work/help.db" --help >"$work/help"
[[ ! -e "$work/help.db" ]]
grep -q '^usage:' "$work/help"
echo 'task board CLI and persistence checks passed'
