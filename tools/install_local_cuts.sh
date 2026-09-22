#!/usr/bin/env bash
set -euo pipefail

# Bootstrap checked local cuts until registry-backed facet install exists.
# Every installed directory comes from a verified archive, not a source symlink.
if [[ $# -ne 1 ]]; then
    echo 'usage: tools/install_local_cuts.sh <project-directory>' >&2
    exit 64
fi
source_root="$(cd "$(dirname "$0")/.." && pwd)"
project="$1"
mkdir -p "$project/cuts"
project="$(cd "$project" && pwd)"
facet="${FACET_BIN:-$source_root/build/facet}"
if [[ -z "${FACET_BIN:-}" ]]; then
    if command -v gmake >/dev/null 2>&1; then
        gmake -s -C "$source_root" facet
    else
        make -s -C "$source_root" facet
    fi
fi
if [[ ! -x "$facet" ]]; then
    echo "facet binary not found: $facet" >&2
    exit 66
fi
export LC_ALL=C
names=()
for directory in "$source_root"/packages/*; do
    if [[ -f "$directory/diamond.cut" ]]; then
        names+=("${directory##*/}")
    fi
done
if [[ ${#names[@]} -eq 0 ]]; then
    echo 'no local cuts selected' >&2
    exit 65
fi
stage="$(mktemp -d "$project/cuts/.facet-local.XXXXXXXX")"
trap 'rm -rf -- "$stage"' EXIT
for name in "${names[@]}"; do
    source="$source_root/packages/$name"
    archive="$stage/$name.tar"
    "$facet" check "$source" >/dev/null
    "$facet" pack "$source" "$archive" >/dev/null
    digest="$(openssl dgst -sha256 "$archive" | awk '{print $NF}')"
    "$facet" verify "$archive" --sha256 "$digest" >/dev/null
    mkdir "$stage/$name"
    tar -xf "$archive" -C "$stage/$name"
done
for name in "${names[@]}"; do
    rm -rf -- "$project/cuts/$name"
    mv -- "$stage/$name" "$project/cuts/$name"
done
printf 'installed %s verified local cuts in %s/cuts\n' "${#names[@]}" "$project"
