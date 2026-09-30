#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
"$diamond" build debug_me.di -o "$work/debug_me" > "$work/build.log"
"$diamond" build solution.di -o "$work/solution" >> "$work/build.log"

check_pause() {
  grep -q '^--- paused at line_total:' "$work/actual"
  grep -qx '  price = 12' "$work/actual"
  grep -qx '  quantity = 3' "$work/actual"
  grep -qx '  total = 15' "$work/actual"
  grep -qx '(press Enter to continue)' "$work/actual"
  [[ $(tail -n 1 "$work/actual") == 'Total: 15 cents' ]]
}
check_bug() {
  printf '\n' | "$@" > "$work/actual"
  check_pause
  "$@" < /dev/null > "$work/actual"
  check_pause
}
check_bug "$diamond" debug_me.di
check_bug "$work/debug_me"
[[ $("$diamond" solution.di) == 'Total: 36 cents' ]]
[[ $("$work/solution") == 'Total: 36 cents' ]]
echo "debug_me smoke test passed"
