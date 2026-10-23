#!/usr/bin/env bash
# Seed reproducibility and failure checks; no network or credentials.
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
diamond="${DIAMOND_BIN:-$root/build/diamond}"
tool="$root/tools/prepare_registry_seed.d6"
expected="$root/docs/registry-launch-inventory.json"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

prepare() { # DESTINATION [options]: prints stderr, returns the tool's status
    "$diamond" "$tool" "$@" 2>&1 >/dev/null
}

prepare "$work/first" --expect "$expected"
prepare "$work/second" --expect "$expected"
diff -r "$work/first" "$work/second" > /dev/null

# An existing destination is rejected and left alone.
saved="$(cat "$work/first/inventory.json")"
if prepare "$work/first" > /dev/null; then echo "an existing destination was overwritten" >&2; exit 1; fi
[[ "$(cat "$work/first/inventory.json")" == "$saved" ]]

# A reviewed inventory that differs is rejected, and no directory is left behind.
awk 'BEGIN { done = 0 } !done && /"sha256":/ { sub(/"sha256": "[0-9a-f]+"/, "\"sha256\": \"0000000000000000000000000000000000000000000000000000000000000000\""); done = 1 } { print }' \
    "$work/first/inventory.json" > "$work/changed.json"
if prepare "$work/failed" --expect "$work/changed.json" > /dev/null; then echo "a changed inventory was accepted" >&2; exit 1; fi
[[ ! -e "$work/failed" ]]

# A selection naming only a cut that has dependencies fails on the missing dependency.
registry_version="$("$diamond" -e "puts(JSON.parse(File.read(\"$root/applications/registry/launch-cuts.json\"))[\"registry\"]); exit(0)")"
printf '{"registry": "%s"}\n' "$registry_version" > "$work/selection.json"
message="$(prepare "$work/failed" --selection "$work/selection.json" || true)"
[[ "$message" == *"dependency missing"* ]] || { echo "unexpected: $message" >&2; exit 1; }
[[ ! -e "$work/failed" ]]

# A selection naming a version that is not the one in the tree fails on identity.
printf '{"registry": "99.0.0"}\n' > "$work/selection.json"
message="$(prepare "$work/failed" --selection "$work/selection.json" || true)"
[[ "$message" == *"identity changed"* ]] || { echo "unexpected: $message" >&2; exit 1; }
[[ ! -e "$work/failed" ]]

echo "seed reproducibility, reviewed digest, destination preservation, and dependency checks passed"
