#!/usr/bin/env bash
# Runs every test/*_test.d6 file and fails if any of them fails. The tests
# load minitest from cuts/, which `facet install` fills.
# DIAMOND_BIN picks a diamond other than the one on PATH.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-diamond}"
if [[ ! -d cuts/minitest ]]; then
    echo "test.sh: run 'facet install' first; it fetches minitest into cuts/" >&2
    exit 1
fi
status=0
for test in test/*_test.d6; do
    echo "== $test"
    "$diamond" "$test" || status=1
done
exit "$status"
