#!/usr/bin/env bash
set -euo pipefail

# Bytecode caching (docs/caching.md) only ever exists inside diamond_run_
# source's own auto-dispatch (src/run_source.c) -- the real `diamond`
# CLI's own entry point, not build/run_cases's diamond_run_source_with_
# template. It can't be exercised from tests/cases/*.di at all for
# exactly that reason (see docs/caching.md's own "what's explicitly not
# covered"), so every check here goes through a real `diamond` subprocess
# in its own temp directory instead, the same shape tests/dap_test.sh/
# tests/exit_test.sh already use for whatever they can't express as an
# ordinary corpus case either.

diamond="$(realpath ./build/diamond)"
count=0

work="$(realpath "$(mktemp -d)")"
trap 'rm -rf "$work"' EXIT

cat > "$work/lib.di" <<'EOF'
def helper(n)
  n * 2
end
EOF
cat > "$work/app.di" <<'EOF'
require "./lib"
helper(21)
EOF

# --- first run: no cache yet -- a miss, followed by a write, and the
# ordinary correct result either way ---
[[ ! -f "$work/app.dic" ]]
out="$(cd "$work" && DIAMOND_TRACE_CACHE=1 "$diamond" app.di 2>&1)"
[[ "$out" == *"cache: miss"*"app.dic"* ]]
[[ "$out" == *"cache: wrote"*"app.dic"* ]]
[[ "$out" == *$'\n'"42" ]]
[[ -f "$work/app.dic" ]]
count=$((count + 1))

# --- second run, source unchanged: a hit, no re-write, same result ---
out="$(cd "$work" && DIAMOND_TRACE_CACHE=1 "$diamond" app.di 2>&1)"
[[ "$out" == *"cache: hit"*"app.dic"* ]]
[[ "$out" != *"cache: wrote"* ]]
[[ "$out" == *$'\n'"42" ]]
count=$((count + 1))

# --- editing only the required file (entry file untouched) still
# invalidates -- the cache key covers the whole expanded bundle, not
# just the one path named on the command line ---
sed -i.bak 's/n \* 2/n * 3/' "$work/lib.di"
out="$(cd "$work" && DIAMOND_TRACE_CACHE=1 "$diamond" app.di 2>&1)"
[[ "$out" == *"cache: miss"* ]]
[[ "$out" == *$'\n'"63" ]]
count=$((count + 1))

# --- a truncated/corrupted .dic falls back to a clean recompile, never
# a crash, and never a wrong result ---
head -c 40 /dev/urandom > "$work/app.dic"
out="$(cd "$work" && DIAMOND_TRACE_CACHE=1 "$diamond" app.di 2>&1)"
[[ "$out" == *"cache: miss"* ]]
[[ "$out" == *$'\n'"63" ]]
count=$((count + 1))

: > "$work/app.dic"
out="$(cd "$work" && DIAMOND_TRACE_CACHE=1 "$diamond" app.di 2>&1)"
[[ "$out" == *"cache: miss"* ]]
[[ "$out" == *$'\n'"63" ]]
count=$((count + 1))

# --- DIAMOND_NO_CACHE=1 skips it entirely -- no trace output, no
# rewritten .dic even though one already exists from the runs above ---
rm -f "$work/app.dic"
out="$(cd "$work" && DIAMOND_NO_CACHE=1 DIAMOND_TRACE_CACHE=1 "$diamond" app.di 2>&1)"
[[ "$out" != *"cache:"* ]]
[[ ! -f "$work/app.dic" ]]
count=$((count + 1))

# --- -e never creates a cache file, anywhere, regardless of cwd ---
rm -f "$work"/*.dic
(cd "$work" && DIAMOND_TRACE_CACHE=1 "$diamond" -e '1 + 1' >/dev/null 2>&1)
[[ -z "$(find "$work" -name '*.dic' 2>/dev/null)" ]]
count=$((count + 1))

echo "$count cache tests passed"
