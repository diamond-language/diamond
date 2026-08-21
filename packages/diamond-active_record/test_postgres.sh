#!/usr/bin/env bash
set -euo pipefail

# Opt-in cross-dialect conformance test for ActiveRecordRepository's visitor
# parameter. Same reasoning and pattern as
# packages/arel/test_postgres_dialect.sh: needs an already-running
# PostgreSQL server, which Diamond can't spin up itself, so this lives here
# instead of tests/cases/ (self-contained, in-memory SQLite) and is never
# run by `make test`/CI. Manages its own throwaway podman container;
# requires podman (skips, not fails, if it isn't installed).
#
# Usage: bash test_postgres.sh
#   (or DIAMOND_BIN=../../build/diamond bash test_postgres.sh)

diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

if ! command -v podman >/dev/null 2>&1; then
    echo "podman not found -- skipping diamond-active_record PostgreSQL conformance test" >&2
    exit 0
fi

container_name="diamond-ar-postgres-test"
port=55435

cleanup() {
    podman stop "$container_name" >/dev/null 2>&1 || true
}
trap cleanup EXIT

podman rm -f "$container_name" >/dev/null 2>&1 || true
podman run --rm -d --name "$container_name" \
    -e POSTGRES_PASSWORD=diamondtest \
    -e POSTGRES_DB=diamond_ar_test \
    -p "127.0.0.1:${port}:5432" \
    docker.io/library/postgres:16-alpine >/dev/null

ready=0
for _ in $(seq 1 30); do
    if podman exec "$container_name" pg_isready -U postgres >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 1
done
if [[ "$ready" != "1" ]]; then
    echo "PostgreSQL container did not become ready in time" >&2
    exit 1
fi

export DIAMOND_PG_TEST_CONNINFO="host=127.0.0.1 port=${port} dbname=diamond_ar_test user=postgres password=diamondtest"

"$diamond" test_postgres.di
