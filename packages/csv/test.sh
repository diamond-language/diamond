#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (`make test-csv-package` from the repo root sets this up).
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

CSV_TEST_DIR="$work" "$diamond" test/csv_test.d6
