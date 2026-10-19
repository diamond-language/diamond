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
"$diamond" "$root/tests/cases/mjolnir_repo_extras.di"
# RETURNING and the read-back path used where a database has none (MySQL, MariaDB UPDATE), both on SQLite; test_dialects.sh runs the live servers.
"$diamond" "$root/tests/cases/mjolnir_dialect_writes.di"
DIAMOND_MJOLNIR_DIALECT=sqlite_no_returning "$diamond" "$root/tests/cases/mjolnir_dialect_writes.di"

echo "5 mjolnir test programs passed"
