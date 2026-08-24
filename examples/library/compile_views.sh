#!/usr/bin/env bash
set -euo pipefail

# Batch-compiles every views/*.html.div into a views/*.html.di next to it
# (packages/div/bin/divc.di's own default suffix-swap output path) --
# packages/div/ROADMAP.md lists a batch-compile convenience as a deferred,
# demand-driven open question; this app is that demand. Run this before
# ../../build/diamond app.di -- app.di's own require lines expect the
# compiled .html.di files to already exist (require is compile-time
# expansion, so they must exist on disk before app.di is compiled, not
# just before it runs).
diamond="${DIAMOND_BIN:-../../build/diamond}"
cd "$(dirname "$0")"

for source in views/*.html.div; do
    "$diamond" ../../packages/div/bin/divc.di "$source"
done
