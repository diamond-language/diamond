#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
"$diamond" build databases.di -o "$work/databases" > "$work/build.log"
printf '%s\n' '{"adapter":"mysql"}' > "$work/invalid.json"
printf '%s\n' '{"adapter":"postgresql","connection":"invalid_connection_option=1"}' > "$work/bad-pg.json"

check() {
  env -u DIAMOND_DATABASE_EXAMPLE_CONFIG "$@" > "$work/skip.out"
  grep -q '^SKIP: set DIAMOND_DATABASE_EXAMPLE_CONFIG' "$work/skip.out"
  for config in "$work/invalid.json" "$work/bad-pg.json" ""; do
    if DIAMOND_DATABASE_EXAMPLE_CONFIG="$config" "$@" > "$work/error.out" 2>&1; then
      echo "configured failure unexpectedly succeeded: $config" >&2
      exit 1
    fi
    if grep -q '^SKIP:' "$work/error.out"; then
      echo "configured failure was skipped" >&2
      exit 1
    fi
  done
  if [[ ${DIAMOND_DATABASE_EXAMPLE_CONFIG+x} ]]; then
    "$@" | tee "$work/live.out"
    grep -q '^PASS:' "$work/live.out"
  fi
}
check "$diamond" databases.di
check "$work/databases"
echo "databases smoke test passed"
