#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (`make test-mustache-package` from the repo root sets this up).
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

"$diamond" test/mustache_test.d6
