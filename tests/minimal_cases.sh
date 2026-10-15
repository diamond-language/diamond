#!/usr/bin/env bash
# Checks a `make MINIMAL=1` build (no SQLite, PostgreSQL, MySQL/MariaDB or zlib support):
# the features it dropped fail with a clear error, and every case that does not use them
# still produces its expected output. Build first: make clean && make MINIMAL=1 debug
set -uo pipefail
cd "$(dirname "$0")/.."
diamond="${DIAMOND_BIN:-build/diamond}"

# 1. Each dropped feature raises an error that names it, instead of crashing or linking.
fail=0
expect_error() {
    local snippet="$1" fragment="$2" output
    output="$(timeout 20 "$diamond" -e "$snippet" 2>&1 </dev/null)"
    if [[ "$output" != *"$fragment"* ]]; then
        echo "FAIL: $snippet" >&2
        echo "  expected an error containing: $fragment" >&2
        echo "  got: $output" >&2
        fail=1
    fi
}
expect_error 'SQLite3.open(":memory:")' "SQLite3 support is not compiled into this build"
expect_error 'PostgreSQL.open("host=localhost")' "PostgreSQL support is not compiled into this build"
expect_error 'MySQL.open("localhost", "u", "p", "d", 3306)' "MySQL support is not compiled into this build"
expect_error 'Gzip.compress("hello")' "Gzip is not compiled into this build"
[[ $fail -eq 0 ]] || exit 1

# 2. Everything that does not use a dropped feature behaves as it does in a full build.
uses_dropped='SQLite3|PostgreSQL|MySQL|Gzip|ActiveRecord|active_record|Mjolnir|mjolnir|Arel|arel|require_cut|database_config|Repository|Jobs\b|jobs'
checked=0
for source in tests/cases/*.di; do
    base="${source%.di}"
    [[ -f "$base.expected" ]] || continue
    grep -Eq "$uses_dropped" "$source" && continue
    env_args=(); flag_args=(); line=
    if [[ -f "$base.env" ]]; then
        while IFS= read -r line; do [[ -n "$line" ]] && env_args+=("$line"); done < "$base.env"
    fi
    if [[ -f "$base.flags" ]]; then
        while IFS= read -r line; do [[ -n "$line" ]] && flag_args+=("$line"); done < "$base.flags"
    fi
    actual="$(timeout 60 env "${env_args[@]}" "$diamond" "${flag_args[@]}" "$source" 2>/dev/null </dev/null)"
    if [[ "$actual" != "$(cat "$base.expected")" ]]; then
        echo "FAIL: $source" >&2
        fail=1
    fi
    checked=$((checked + 1))
done
echo "minimal build: $checked cases checked"
exit $fail
