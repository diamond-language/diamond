#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
diamond="${DIAMOND_BIN:-$root/build/diamond}"
export DIAMOND_NO_CACHE=1
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
bash "$root/tools/install_local_cuts.sh" "$work" >/dev/null
cd "$work"
python3 "$root/examples/web_apps_test.py" "$diamond"
for app in task_board bookmarks job_service; do
  entry=web.d6
  [[ "$app" != job_service ]] || entry=service.d6
  "$diamond" build "$root/examples/$app/$entry" -o "$work/$app" >"$work/build.log" 2>&1 || { cat "$work/build.log"; exit 1; }
done
python3 "$root/examples/web_apps_test.py" "$diamond" "$work"
