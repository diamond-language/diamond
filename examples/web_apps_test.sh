#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
diamond="${DIAMOND_BIN:-$root/build/diamond}"
export DIAMOND_NO_CACHE=1
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
bash "$root/tools/install_local_cuts.sh" "$work" >/dev/null
cd "$work"
checks() {
  rm -rf "$work/run"
  mkdir -p "$work/run/launch"
  ln -s "$work/cuts" "$work/run/launch/cuts"
  ln -s "$root/examples" "$work/run/launch/examples"
  "$diamond" "$root/examples/web_apps_test.d6" "$diamond" "$root" "$work/run" "$@"
}
checks
for app in task_board bookmarks job_service; do
  entry=web.d6
  [[ "$app" != job_service ]] || entry=service.d6
  "$diamond" build "$root/examples/$app/$entry" -o "$work/$app" >"$work/build.log" 2>&1 || { cat "$work/build.log"; exit 1; }
done
checks "$work"
