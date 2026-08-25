#!/usr/bin/env bash
set -euo pipefail

# App-local convenience wrapper around Div's recursive clean batch compiler.
cd "$(dirname "$0")"
DIAMOND_BIN="${DIAMOND_BIN:-../../build/diamond}" ../../packages/div/bin/divc_all.sh lib/views
