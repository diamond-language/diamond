#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (`make test-markdown-package` from the repo root sets this up).
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

"$diamond" test/markdown_test.d6
