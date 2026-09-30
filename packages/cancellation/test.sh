#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export DIAMOND_NO_CACHE=1
timeout 30 "$diamond" test/cancellation_test.di
"$diamond" build test/cancellation_test.di -o "$work/cancellation-test" > "$work/build.log"
timeout 30 "$work/cancellation-test"
