#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
diamond="${DIAMOND_BIN:-$PWD/build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export DIAMOND_NO_CACHE=1
bash tools/install_local_cuts.sh . > "$work/cuts.log"
"$diamond" examples/job_service/smoke_test.d6 "$diamond" examples/job_service/service.d6
"$diamond" build examples/job_service/service.d6 -o "$work/service" > "$work/build.log"
"$diamond" examples/job_service/smoke_test.d6 "$work/service"
