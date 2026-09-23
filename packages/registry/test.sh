#!/usr/bin/env bash
set -euo pipefail

diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"
source_root="$(cd ../.. && pwd)"
root="$(mktemp -d)"
test_project="$(mktemp -d)"
trap 'rm -rf "$root" "$test_project"' EXIT
"$source_root/tools/install_local_cuts.sh" "$test_project" >/dev/null
cp -R . "$test_project/registry_source"
cd "$test_project"

output="$($diamond -e "require_cut \"registry\"
db = SQLite3.open(\":memory:\")
Registry::Schema.apply(db)
Registry::Schema.apply(db)
store = Registry::BlobStore.new(\"$root\")
bytes = \"archive bytes\"
digest = Digest.sha256(bytes)
size = store.put(digest, bytes)
same = store.read(digest)
rows = db.query(\"SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'releases'\")
\"#{size}|#{same}|#{rows.length()}|#{store.contains?(digest)}\"")"

[[ "$output" == "13|archive bytes|1|true" ]]
archive_digest="$($diamond -e 'puts(Digest.sha256("archive bytes"))')"

if "$diamond" -e "require \"$(pwd)/lib/registry\"; Registry::BlobStore.new(\"$root\").put(\"$(printf '0%.0s' {1..64})\", \"wrong\")" >/dev/null 2>&1; then
    echo "digest mismatch was accepted" >&2
    exit 1
fi

if "$diamond" -e "require_cut \"registry\"; Registry::BlobStore.new(\"$root\").put(\"$archive_digest\", \"archive bytes\")" >/dev/null 2>&1; then
    echo "duplicate digest was replaced" >&2
    exit 1
fi

echo "registry package tests passed"
