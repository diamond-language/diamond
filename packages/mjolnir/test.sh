#!/usr/bin/env bash
set -euo pipefail

diamond="${DIAMOND_BIN:-diamond}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
test_project="$(mktemp -d)"
trap 'rm -rf "$test_project"' EXIT
"$root/tools/install_local_cuts.sh" "$test_project" >/dev/null
cd "$test_project"

"$diamond" "$root/tests/cases/mjolnir_changeset.di"
"$diamond" "$root/tests/cases/mjolnir_repo.di"

echo "2 mjolnir test programs passed"
