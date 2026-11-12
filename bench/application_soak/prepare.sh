#!/usr/bin/env bash
# Native local test builds. Each revision gets an independent compiler/AOT kit.
set -euo pipefail
root=$(git rev-parse --show-toplevel)
workspace=${1:?workspace directory required}
mkdir -p "$workspace"
workspace=$(realpath "$workspace")
[[ ! -e $workspace/revisions.txt ]] || { echo 'Use a fresh comparison workspace'; exit 1; }
app_root=$(realpath "$root/..")
candidate=$(git -C "$root" rev-parse 1.0)
baseline=$(git -C "$root" rev-parse 'v1.0.0-rc3^{}')
pheint=$(git -C "$app_root/pheint" rev-parse main)
skindicate=$(git -C "$app_root/SkindicateV3" rev-parse main)
bench_revision=daf7e8d85798c6aa52a85bf24b818512fa7a2939
printf 'baseline=%s\ncandidate=%s\npheint=%s\nskindicate=%s\npheint_bench=%s\n' \
    "$baseline" "$candidate" "$pheint" "$skindicate" "$bench_revision" > "$workspace/revisions.txt"
sha256sum "$root/bench/application_soak/instrument.rb" > "$workspace/instrument.sha256"
for variant in rc3 candidate; do
    revision=$baseline
    if [[ $variant == candidate ]]; then revision=$candidate; fi
    runtime=$workspace/$variant/diamond
    mkdir -p "$runtime"
    git -C "$root" archive "$revision" | tar -xf - -C "$runtime"
    ruby "$root/bench/application_soak/instrument.rb" "$runtime"
    make -C "$runtime" -j4 PGO=0 CFLAGS_RELEASE='-O3 -DNDEBUG -march=x86-64-v3' \
        CFLAGS_DEBUG='-O2 -g -march=x86-64-v3' release facet > "$workspace/$variant/runtime-build.log" 2>&1
    make -C "$runtime" -j4 AOT_LTO=1 \
        AOT_LTO_CFLAGS='$(CFLAGS_COMMON) -O3 -g0 -flto -ffat-lto-objects -march=x86-64-v3' \
        aot-kit >> "$workspace/$variant/runtime-build.log" 2>&1
    echo "$variant runtime and LTO kit built"
done
mkdir -p "$workspace/apps/pheint" "$workspace/apps/SkindicateV3" "$workspace/artifacts"
git -C "$app_root/pheint" archive "$pheint" | tar -xf - -C "$workspace/apps/pheint"
git -C "$app_root/pheint" archive "$bench_revision" bench | tar -xf - -C "$workspace/apps/pheint"
git -C "$app_root/SkindicateV3" archive "$skindicate" | tar -xf - -C "$workspace/apps/SkindicateV3"
ln -s "$workspace/candidate/diamond" "$workspace/apps/diamond"
(cd "$workspace/apps/pheint" && "$workspace/candidate/diamond/build/facet" install) > "$workspace/pheint-cuts.log" 2>&1
(cd "$workspace/apps/SkindicateV3" && bash tools/setup_cuts.sh && \
    DIAMOND_BIN="$workspace/candidate/diamond/build/d6" bash compile_views.sh) > "$workspace/skindicate-cuts.log" 2>&1
for variant in rc3 candidate; do
    runtime=$workspace/$variant/diamond
    for app in pheint SkindicateV3; do
        entry=app.d6
        if [[ $app == pheint ]]; then entry=app.di; fi
        (cd "$workspace/apps/$app" && DIAMOND_AOT_KIT="$runtime/build/aot-kit" \
            "$runtime/build/d6" build --lto "$entry" -o "$workspace/artifacts/$app-$variant") \
            > "$workspace/$app-$variant-build.log" 2>&1
        sha256sum "$workspace/artifacts/$app-$variant" >> "$workspace/artifacts/SHA256SUMS"
        echo "$app $variant standalone built"
    done
done
echo "All local comparison binaries built in $workspace/artifacts"
