#!/usr/bin/env bash
# Runs the pipeline through the interpreter and as a `diamond build` binary
# and checks the whole story: good items flow through all three sandboxed
# stages in order, a runaway item is stopped by its budget, a hostile item is
# denied the network, a transient crash is retried after the supervisor
# restarts the worker, and a permanently crashing item is dead-lettered after
# max_attempts. A second manifest withholds the `filesystem` grant to show
# that the allow-list, not the stage, decides what a stage may do.
# Set DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" build pipeline.di -o "$work/pipeline" > "$work/build.log"

status_of() { local s=0; "$@" > /dev/null 2>&1 || s=$?; echo "$s"; }

for vm in "$diamond pipeline.di $diamond" "$work/pipeline $diamond"; do
  $vm | cmp testdata/run.expected -
  $vm testdata/no_filesystem.json | cmp testdata/no_filesystem.expected -
  [[ "$(status_of $vm testdata/missing.json)" == 66 ]]
done

[[ "$(status_of "$diamond" pipeline.di)" == 64 ]]

echo "pipeline smoke test passed"
