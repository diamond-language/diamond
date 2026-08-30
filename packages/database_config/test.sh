#!/usr/bin/env bash
set -euo pipefail

diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"
DIAMOND_DATABASE_CONFIG_TEST_PASSWORD="test-secret" \
  "$diamond" test/config_test.di
