#!/usr/bin/env bash
# Rebuild measurement snapshots after changing the common observation overlay.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(git -C "$here" rev-parse --show-toplevel)
workspace=$(realpath "${1:?workspace directory required}")
sha256sum "$here/instrument.rb" > "$workspace/instrument.sha256"
: > "$workspace/artifacts/SHA256SUMS"
for variant in rc3 candidate; do
    key=baseline
    if [[ $variant == candidate ]]; then key=candidate; fi
    revision=$(sed -n "s/^$key=//p" "$workspace/revisions.txt")
    runtime=$workspace/$variant/diamond
    for source in vm.c vm_gc.c; do git -C "$root" show "$revision:src/$source" > "$runtime/src/$source"; done
    ruby "$here/instrument.rb" "$runtime"
    make -C "$runtime" -j4 PGO=0 CFLAGS_RELEASE='-O3 -DNDEBUG -march=x86-64-v3' \
        CFLAGS_DEBUG='-O2 -g -march=x86-64-v3' release facet > "$workspace/$variant/runtime-rebuild.log" 2>&1
    make -C "$runtime" -j4 AOT_LTO=1 \
        AOT_LTO_CFLAGS='$(CFLAGS_COMMON) -O3 -g0 -flto -ffat-lto-objects -march=x86-64-v3' \
        aot-kit >> "$workspace/$variant/runtime-rebuild.log" 2>&1
    for app in pheint SkindicateV3; do
        entry=app.d6
        if [[ $app == pheint ]]; then entry=app.di; fi
        (cd "$workspace/apps/$app" && DIAMOND_AOT_KIT="$runtime/build/aot-kit" \
            "$runtime/build/d6" build --lto "$entry" -o "$workspace/artifacts/$app-$variant") \
            > "$workspace/$app-$variant-rebuild.log" 2>&1
        sha256sum "$workspace/artifacts/$app-$variant" >> "$workspace/artifacts/SHA256SUMS"
    done
    echo "$variant apps rebuilt with GC checkpoints"
done
