#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${SOAK_RESULT_DIR:?result directory required}
mkdir -p "$out"
database=${PHEINT_DB:-${SKINDICATE_DB:?database required}}
role=server
if [[ $database == /dev/shm/* ]]; then role=seed; fi
stem=$out/$role-$$
ruby "$here/sample.rb" "$$" "$stem.memory.jsonl" "${SOAK_PHASE_FILE:?phase file required}" "$database" &
export SOAK_DIAGNOSTICS=1 DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=32 LOG_LEVEL=off
exec taskset -c "${SOAK_SERVER_CPUS:-0,2,4,6}" "${SOAK_BINARY:?application binary required}" >> "$stem.log" 2>&1
