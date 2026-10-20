#!/usr/bin/env bash
set -euo pipefail

# Opt-in: runs tests/cases/mjolnir_dialect_writes.di against live MariaDB, MySQL and PostgreSQL servers in throwaway podman containers
# (the same shape as packages/arel/test_*_dialect.sh). The default dialects of that file -- SQLite with and without RETURNING -- run in
# every `make test`; this checks that the read-back path and the constraint-message matching hold on the databases they are for.
#
# Usage: bash test_dialects.sh [mariadb|mysql|postgres ...]   (default: all three)
#   (or DIAMOND_BIN=../../build/diamond bash test_dialects.sh)

diamond="${DIAMOND_BIN:-diamond}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
test_file="$root/tests/cases/mjolnir_dialect_writes.di"
# The test requires the mjolnir and arel cuts, so run it from a project that has them installed.
test_project="$(mktemp -d)"
"$root/tools/install_local_cuts.sh" "$test_project" >/dev/null
cd "$test_project"

if ! command -v podman >/dev/null 2>&1; then
    echo "podman not found -- skipping the live Mjolnir dialect tests" >&2
    exit 0
fi

wanted=("$@")
[[ ${#wanted[@]} -gt 0 ]] || wanted=(mariadb mysql postgres)
container="diamond-mjolnir-dialect-test"
cleanup() { podman stop "$container" >/dev/null 2>&1 || true; }
trap 'cleanup; cd /; rm -rf "$test_project"' EXIT

wait_for() { # command...
    for _ in $(seq 1 60); do
        if "$@" >/dev/null 2>&1; then return 0; fi
        sleep 1
    done
    return 1
}

for dialect in "${wanted[@]}"; do
    cleanup
    podman rm -f "$container" >/dev/null 2>&1 || true
    case "$dialect" in
        mariadb|mysql)
            port=33071
            if [[ "$dialect" == mariadb ]]; then image=docker.io/library/mariadb:11; client=mariadb; else image=docker.io/library/mysql:8.4; client=mysql; fi
            podman run --rm -d --name "$container" -e MARIADB_ROOT_PASSWORD=diamondtest -e MYSQL_ROOT_PASSWORD=diamondtest \
                -e MARIADB_DATABASE=diamond_mjolnir_test -e MYSQL_DATABASE=diamond_mjolnir_test \
                -p "127.0.0.1:${port}:3306" "$image" >/dev/null
            wait_for podman exec "$container" "$client" -uroot -pdiamondtest -h127.0.0.1 diamond_mjolnir_test -e "SELECT 1" \
                || { echo "$dialect did not become ready" >&2; exit 1; }
            export DIAMOND_MYSQL_TEST_HOST=127.0.0.1 DIAMOND_MYSQL_TEST_USER=root DIAMOND_MYSQL_TEST_PASSWORD=diamondtest
            export DIAMOND_MYSQL_TEST_DATABASE=diamond_mjolnir_test DIAMOND_MYSQL_TEST_PORT="$port"
            ;;
        postgres)
            port=33072
            podman run --rm -d --name "$container" -e POSTGRES_PASSWORD=diamondtest -e POSTGRES_DB=diamond_mjolnir_test \
                -p "127.0.0.1:${port}:5432" docker.io/library/postgres:16-alpine >/dev/null
            wait_for podman exec "$container" pg_isready -U postgres -d diamond_mjolnir_test \
                || { echo "postgres did not become ready" >&2; exit 1; }
            sleep 2
            export DIAMOND_PG_TEST_CONNINFO="host=127.0.0.1 port=${port} dbname=diamond_mjolnir_test user=postgres password=diamondtest"
            ;;
        *) echo "unknown dialect '$dialect'" >&2; exit 64 ;;
    esac
    echo "== $dialect"
    DIAMOND_MJOLNIR_DIALECT="$dialect" "$diamond" "$test_file"
done
echo "mjolnir live dialect tests passed"
