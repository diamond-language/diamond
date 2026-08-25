#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
DIAMOND_BIN="$diamond" bash compile_views.sh
DIAMOND_ENV=test "$diamond" smoke_test.di
