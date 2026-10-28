#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
diamond="${DIAMOND_BIN:-$root/build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
bash "$root/tools/install_local_cuts.sh" "$work" >/dev/null
cd "$work"
"$diamond" "$root/examples/task_board/store_test.d6"
"$diamond" "$root/examples/task_board/editor_test.d6"
"$diamond" "$root/examples/task_board/board_test.d6"
"$diamond" "$root/examples/task_board/migration_test.d6" "$work/legacy.db"
run() { "$diamond" "$root/examples/task_board/app.d6" --db "$work/board.db" "$@"; }
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
reject 64 list --search
reject 64 list --search one --search two
reject 66 notes 999 Missing
reject 66 show 999
reject 64 nonsense
# Each command opens a fresh process. Rejected edits preserve stored values.
[[ "$(run list --project 1)" == '#1 [doing] Diamond / Terminal board' ]]
[[ "$(run move 1 done)" == 'task #1 [done]: Terminal board' ]]
[[ "$(run list --status doing)" == 'No tasks.' ]]
[[ "$(run list --project 1)" == '#1 [done] Diamond / Terminal board' ]]
[[ "$(run notes 1 $'Remember benchmarks\nKeep results')" == 'task #1 [done]: Terminal board' ]]
[[ "$(run show 1)" == $'task #1 [done]: Terminal board\nNotes:\nRemember benchmarks\nKeep results' ]]
[[ "$(run list --project 1 --status done --search BENCHMARKS)" == '#1 [done] Diamond / Terminal board' ]]
[[ "$(run list --search Diamond)" == '#1 [done] Diamond / Terminal board' ]]
[[ "$(run list --search '[missing]')" == 'No tasks.' ]]
run notes 1 '' >/dev/null
[[ "$(run list --search benchmarks)" == 'No tasks.' ]]
BOARD_DATABASE_PATH="$work/unused.db" run projects >"$work/projects"
[[ ! -e "$work/unused.db" ]]
grep -q '#1 Diamond' "$work/projects"
"$diamond" "$root/examples/task_board/app.d6" --db "$work/help.db" --help >"$work/help"
[[ ! -e "$work/help.db" ]]
grep -q '^usage:' "$work/help"
status=0
bash "$root/examples/task_board/board" --db "$work/no-terminal.db" </dev/null >"$work/out" 2>"$work/err" || status=$?
[[ "$status" == 64 && ! -e "$work/no-terminal.db" ]]
grep -q 'needs a terminal' "$work/err"
if command -v python3 >/dev/null 2>&1; then
  python3 "$root/examples/task_board/terminal_test.py" "$work/terminal.db"
else
  echo 'PTY checks skipped (python3 unavailable); controller checks passed above'
fi
echo 'task board CLI and persistence checks passed'
