#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
workspace=$(realpath "${1:?workspace directory required}")
root=${SOAK_SOURCE_ROOT:-$(git -C "$here" rev-parse --show-toplevel)}
bench_revision=$(sed -n 's/^pheint_bench=//p' "$workspace/revisions.txt")
git -C "$root/../pheint" archive "$bench_revision" bench | tar -xf - -C "$workspace/apps/pheint"
ruby "$here/pheint_soak.rb" "$workspace/apps/pheint/bench/bench.di"
cp "$here/pheint_post.di" "$workspace/apps/pheint/bench/soak_post.di"
mkdir -p "$workspace/apps/SkindicateV3/bench"
cp "$here/skindicate.d6" "$workspace/apps/SkindicateV3/bench/soak.d6"
