#!/usr/bin/env bash
# Runs the plugin host through the interpreter and as a `diamond build`
# binary, and checks that each plugin in manifest.json is classified the
# way its own design intends: the legitimate plugin succeeds, an untrusted
# plugin that doesn't handle denial crashes, the same plugin granted
# `filesystem` succeeds, a plugin that rescues the denial itself degrades
# gracefully, and a runaway plugin is stopped by its instruction budget.
# Set DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

check_output() {
    local out="$1"
    grep -qx 'wordcount' "$out"
    grep -q '^  ok             words=9 vowels=11 longest=quick$' "$out"
    grep -qx 'peek (untrusted)' "$out"
    grep -q '^  sandbox_denied .*sandbox denies File.open$' "$out"
    grep -qx 'peek (trusted reader)' "$out"
    grep -q '^  ok             first line: Only a plugin' "$out"
    grep -qx 'tidy (untrusted)' "$out"
    grep -q '^  ok             no filesystem access; skipping' "$out"
    grep -qx 'spinloop' "$out"
    grep -q '^  resource_limit .*resource limit exceeded$' "$out"
}

"$diamond" host.di "$diamond" > "$work/interpreted.txt"
check_output "$work/interpreted.txt"

"$diamond" build host.di -o "$work/host" > "$work/build.log"
"$work/host" "$diamond" > "$work/compiled.txt"
check_output "$work/compiled.txt"

# The host itself must exit 0 -- every plugin above is either meant to
# succeed or meant to be caught and classified, none of them "crashed"
# (the one status that would make the host report failure).
"$diamond" host.di "$diamond" > /dev/null

echo "plugins smoke test passed"
