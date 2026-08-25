#!/usr/bin/env bash
set -euo pipefail

diamond="${DIAMOND_BIN:-diamond}"
root="$(cd "$(dirname "$0")/../.." && pwd)"

"$diamond" "$root/tests/cases/graphsql_mapping.di"
"$diamond" "$root/tests/cases/graphsql_resolver.di"
"$diamond" "$root/tests/cases/graphsql_graphql_integration.di"

echo "3 graphsql test programs passed"
