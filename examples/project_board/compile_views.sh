#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
DIAMOND_BIN="${DIAMOND_BIN:-../../build/diamond}" ../../packages/div/bin/divc_all.sh views
