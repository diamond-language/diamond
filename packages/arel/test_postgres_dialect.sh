#!/usr/bin/env bash
set -euo pipefail

# Opt-in conformance test for ArelPostgreSQLVisitor. Unlike every fixture
# under tests/cases/ (self-contained, in-memory SQLite), this needs an
# already-running PostgreSQL server, which Diamond can't spin up itself the
# way it can a local TCPServer -- so it lives here instead of in the
# automatic corpus and is never run by `make test`/CI. Manages its own
# throwaway podman container; requires podman (skips, not fails, if it
# isn't installed).
#
# Usage: bash test_postgres_dialect.sh
#   (or DIAMOND_BIN=../../build/diamond bash test_postgres_dialect.sh)

diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

if ! command -v podman >/dev/null 2>&1; then
    echo "podman not found -- skipping ArelPostgreSQLVisitor conformance test" >&2
    exit 0
fi

container_name="diamond-arel-postgres-dialect-test"
port=55433

cleanup() {
    podman stop "$container_name" >/dev/null 2>&1 || true
}
trap cleanup EXIT

podman rm -f "$container_name" >/dev/null 2>&1 || true
podman run --rm -d --name "$container_name" \
    -e POSTGRES_PASSWORD=diamondtest \
    -e POSTGRES_DB=diamond_arel_test \
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

export DIAMOND_PG_TEST_CONNINFO="host=127.0.0.1 port=${port} dbname=diamond_arel_test user=postgres password=diamondtest"

"$diamond" test_postgres_dialect.di
