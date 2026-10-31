#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
diamond="${DIAMOND_BIN:-$root/build/diamond}"
export DIAMOND_NO_CACHE=1
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
bash "$root/tools/install_local_cuts.sh" "$work" >/dev/null
cd "$work"
"$diamond" build "$root/examples/site_checker/check.d6" -o "$work/check" >"$work/build.log" 2>&1 || { cat "$work/build.log"; exit 1; }
mkdir "$work/run"
"$diamond" "$root/examples/site_checker/smoke_test.d6" "$diamond" "$root" "$work/run" "$work/check"
