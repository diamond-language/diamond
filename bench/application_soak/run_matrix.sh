#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
workspace=$(realpath "${1:?workspace required}")
bash "$here/configure.sh" "$workspace"
# Reverse the revision order between worker counts to reduce order bias.
for app in SkindicateV3 pheint; do
    for variant in candidate rc3; do bash "$here/run_cell.sh" "$workspace" "$app" "$variant" 1; done
    for variant in rc3 candidate; do bash "$here/run_cell.sh" "$workspace" "$app" "$variant" 4; done
done
ruby "$here/summarize.rb" "$workspace"
