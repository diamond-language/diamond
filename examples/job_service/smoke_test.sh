#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
diamond="${DIAMOND_BIN:-$PWD/build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export DIAMOND_NO_CACHE=1
bash tools/install_local_cuts.sh . > "$work/cuts.log"
python3 examples/job_service/smoke_test.py "$diamond" examples/job_service/service.di
"$diamond" build examples/job_service/service.di -o "$work/service" > "$work/build.log"
python3 examples/job_service/smoke_test.py "$work/service"
