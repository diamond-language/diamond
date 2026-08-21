#!/usr/bin/env bash
set -euo pipefail

# Opt-in conformance test for Arel::MariaDBVisitor -- same reasoning as
# test_postgres_dialect.sh: needs an already-running server Diamond can't
# spin up itself, so it lives here rather than in tests/cases/ and is never
# run by `make test`/CI. Manages its own throwaway podman container;
# requires podman (skips, not fails, if it isn't installed).
#
# Usage: bash test_mariadb_dialect.sh
#   (or DIAMOND_BIN=../../build/diamond bash test_mariadb_dialect.sh)

diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

if ! command -v podman >/dev/null 2>&1; then
    echo "podman not found -- skipping Arel::MariaDBVisitor conformance test" >&2
    exit 0
fi

container_name="diamond-arel-mariadb-dialect-test"
port=33063

cleanup() {
    podman stop "$container_name" >/dev/null 2>&1 || true
}
trap cleanup EXIT

podman rm -f "$container_name" >/dev/null 2>&1 || true
podman run --rm -d --name "$container_name" \
    -e MARIADB_ROOT_PASSWORD=diamondtest \
    -e MARIADB_DATABASE=diamond_arel_test \
    -p "127.0.0.1:${port}:3306" \
    docker.io/library/mariadb:11 >/dev/null

ready=0
for _ in $(seq 1 30); do
    if podman exec "$container_name" mariadb -uroot -pdiamondtest -e "SELECT 1" >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 1
done
if [[ "$ready" != "1" ]]; then
    echo "MariaDB container did not become ready in time" >&2
    exit 1
fi

# MySQL.open takes discrete arguments rather than a single conninfo String
# (see docs/io.md), so these are passed as separate env vars rather than
# one DIAMOND_..._CONNINFO string the way test_postgres_dialect.sh does.
export DIAMOND_MYSQL_TEST_HOST="127.0.0.1"
export DIAMOND_MYSQL_TEST_USER="root"
export DIAMOND_MYSQL_TEST_PASSWORD="diamondtest"
export DIAMOND_MYSQL_TEST_DATABASE="diamond_arel_test"
export DIAMOND_MYSQL_TEST_PORT="${port}"

"$diamond" test_mariadb_dialect.di
