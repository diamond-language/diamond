#!/usr/bin/env bash
set -euo pipefail

bench_dir="$(cd "$(dirname "$0")" && pwd)"
repo_dir="$(cd "$bench_dir/../.." && pwd)"
config="${DATABASE_BENCH_CONFIG:-$bench_dir/config.json}"
diamond="${DIAMOND_BIN:-$repo_dir/build/diamond}"
results="${DATABASE_BENCH_RESULTS:-$bench_dir/results.jsonl}"
engines="${DATABASE_BENCH_ENGINES:-postgresql mariadb mysql}"

for command in podman jq; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "$command is required" >&2
        exit 1
    fi
done
if [[ ! -x "$diamond" ]]; then
    echo "$diamond not found -- run 'make release' first" >&2
    exit 1
fi

rows="$(jq -er '._benchmark.rows' "$config")"
warmup="$(jq -er '._benchmark.warmup_iterations' "$config")"
iterations="$(jq -er '._benchmark.iterations' "$config")"
repeats="$(jq -er '._benchmark.repeats' "$config")"
cpus="$(jq -er '._benchmark.cpus' "$config")"
memory="$(jq -er '._benchmark.memory' "$config")"
pids_limit="$(jq -er '._benchmark.pids_limit' "$config")"

active_container=""
cleanup() {
    if [[ -n "$active_container" && "${KEEP_CONTAINERS:-0}" != "1" ]]; then
        podman rm -f "$active_container" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT INT TERM

mkdir -p "$(dirname "$results")"
if [[ "${DATABASE_BENCH_APPEND:-0}" != "1" ]]; then
    : >"$results"
fi

container_value() {
    local engine="$1" key="$2"
    jq -er --arg engine "$engine" --arg key "$key" '._containers[$engine][$key]' "$config"
}

database_value() {
    local engine="$1" key="$2"
    jq -er --arg engine "$engine" --arg key "$key" '.[$engine][$key]' "$config"
}

start_database() {
    local engine="$1" image name host_port internal_port data_dir database user password
    image="$(container_value "$engine" image)"
    name="$(container_value "$engine" container)"
    internal_port="$(container_value "$engine" internal_port)"
    data_dir="$(container_value "$engine" data_dir)"
    host_port="$(database_value "$engine" port)"
    database="$(database_value "$engine" database)"
    user="$(database_value "$engine" user)"
    password="$(database_value "$engine" password)"

    podman rm -f "$name" >/dev/null 2>&1 || true
    active_container="$name"

    local -a environment
    if [[ "$engine" == "postgresql" ]]; then
        environment=(-e "POSTGRES_DB=$database" -e "POSTGRES_USER=$user" -e "POSTGRES_PASSWORD=$password")
    elif [[ "$engine" == "mariadb" ]]; then
        environment=(-e "MARIADB_DATABASE=$database" -e "MARIADB_USER=$user" -e "MARIADB_PASSWORD=$password" -e "MARIADB_ROOT_PASSWORD=$password-root")
    else
        environment=(-e "MYSQL_DATABASE=$database" -e "MYSQL_USER=$user" -e "MYSQL_PASSWORD=$password" -e "MYSQL_ROOT_PASSWORD=$password-root")
    fi

    echo "starting $engine ($image) on 127.0.0.1:$host_port" >&2
    podman run --rm -d --name "$name" \
        --cpus "$cpus" --memory "$memory" --pids-limit "$pids_limit" \
        --tmpfs "$data_dir:rw,size=512m" \
        -p "127.0.0.1:$host_port:$internal_port" \
        "${environment[@]}" "$image" >/dev/null

    local ready=0
    for _ in $(seq 1 90); do
        if [[ "$engine" == "postgresql" ]]; then
            podman exec "$name" pg_isready -U "$user" -d "$database" >/dev/null 2>&1 && ready=1
        elif [[ "$engine" == "mariadb" ]]; then
            podman exec "$name" mariadb-admin ping -u"$user" -p"$password" --silent >/dev/null 2>&1 && ready=1
        else
            podman exec "$name" mysqladmin ping -u"$user" -p"$password" --silent >/dev/null 2>&1 && ready=1
        fi
        [[ "$ready" == "1" ]] && break
        sleep 1
    done
    if [[ "$ready" != "1" ]]; then
        podman logs "$name" >&2
        echo "$engine did not become ready" >&2
        exit 1
    fi
    # MySQL-compatible images briefly run an initialization server before
    # replacing it with the final listener. An internal ping can hit that
    # temporary process, so require a short stable period before benchmarking.
    sleep 3
}

server_version() {
    local engine="$1" name
    name="$(container_value "$engine" container)"
    if [[ "$engine" == "postgresql" ]]; then
        podman exec "$name" postgres --version
    elif [[ "$engine" == "mariadb" ]]; then
        podman exec "$name" mariadbd --version
    else
        podman exec "$name" mysqld --version
    fi
}

for engine in $engines; do
    start_database "$engine"
    version="$(server_version "$engine")"
    for run in $(seq 1 "$repeats"); do
        "$diamond" "$bench_dir/benchmark.di" "$config" "$engine" \
            "$rows" "$warmup" "$iterations" \
            | jq -c --arg version "$version" --argjson run "$run" \
                --arg cpus "$cpus" --arg memory "$memory" \
                '. + {run: $run, server_version: $version, container_cpus: $cpus, container_memory: $memory}' \
            | tee -a "$results"
    done
    cleanup
    active_container=""
done

echo "results written to $results" >&2
