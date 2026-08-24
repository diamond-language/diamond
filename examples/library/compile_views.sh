#!/usr/bin/env bash
set -euo pipefail

# Batch-compiles every views/*.html.div into views/.cache/*.html.di
# (packages/div/bin/divc.di's own default `.cache/`-subdirectory output
# path -- see packages/div/README.md's "Compiling a template") --
# packages/div/ROADMAP.md lists a batch-compile convenience as a deferred,
# demand-driven open question; this app is that demand. Run this before
# ../../build/diamond app.di -- app.di's own require lines expect the
# compiled .html.di files to already exist (require is compile-time
# expansion, so they must exist on disk before app.di is compiled, not
# just before it runs).
#
# Wipes views/.cache/ first and rebuilds every view fresh, rather than
# only compiling whatever's new or changed -- the translator is fast
# enough that a clean rebuild costs nothing noticeable, and it's the only
# way to guarantee there's never a stale compiled file left behind (a
# renamed or deleted .html.div source, say) silently still being used.
diamond="${DIAMOND_BIN:-../../build/diamond}"
cd "$(dirname "$0")"

rm -rf views/.cache

for source in views/*.html.div; do
    "$diamond" ../../packages/div/bin/divc.di "$source"
done
