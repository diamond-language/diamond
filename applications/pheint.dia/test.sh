#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
bash ../../tools/install_local_cuts.sh . >/dev/null
DIAMOND_ENV=test "$diamond" setup_db.di >/tmp/pheint-test-setup.ndjson
DIAMOND_ENV=test "$diamond" smoke_test.di
