#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
workspace=$(realpath "${1:?workspace required}")
export SOAK_SOURCE_ROOT
SOAK_SOURCE_ROOT=$(git -C "$here" rev-parse --show-toplevel)
[[ ! -e $workspace/harness ]] || { echo 'Frozen harness already exists'; exit 1; }
cp -a "$here" "$workspace/harness"
(cd "$workspace/harness" && sha256sum *) > "$workspace/harness-SHA256SUMS"
find "$workspace/apps/pheint/cuts" "$workspace/apps/SkindicateV3/cuts" -type f \
    ! -name '*.dic' ! -name '*.d6c' -print0 | sort -z | xargs -0 sha256sum > "$workspace/cuts-SHA256SUMS"
{ uname -a; uptime; free -m; lscpu; } > "$workspace/machine.txt"
exec bash "$workspace/harness/run_matrix.sh" "$workspace"
