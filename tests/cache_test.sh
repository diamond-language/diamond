#!/usr/bin/env bash
set -euo pipefail

# Bytecode caching (docs/caching.md) only ever exists inside diamond_run_
# source's own auto-dispatch (src/run_source.c) -- the real `diamond`
# CLI's own entry point, not build/run_cases's diamond_run_source_with_
# template. It can't be exercised from tests/cases/*.d6 at all for
# exactly that reason (see docs/caching.md's own "what's explicitly not
# covered"), so every check here goes through a real `diamond` subprocess
# in its own temp directory instead, the same shape tests/dap_test.sh/
# tests/exit_test.sh already use for whatever they can't express as an
# ordinary corpus case either.

diamond="$(realpath ./build/diamond)"
# The fixtures below assert through their final top-level value, which a
# script run prints only with DIAMOND_PRINT_RESULT set.
export DIAMOND_PRINT_RESULT=1
repo_root="$(pwd)"
count=0

work="$(realpath "$(mktemp -d)")"
trap 'rm -rf "$work"' EXIT

cat > "$work/lib.d6" <<'EOF'
def helper(n)
  n * 2
end
EOF
cat > "$work/app.d6" <<'EOF'
require "./lib"
helper(21)
EOF

# --- first run: no cache yet -- a miss, followed by a write, and the
# ordinary correct result either way ---
[[ ! -f "$work/app.d6c" ]]
out="$(cd "$work" && DIAMOND_TRACE_CACHE=1 "$diamond" app.d6 2>&1)"
[[ "$out" == *"cache: miss"*"app.d6c"* ]]
[[ "$out" == *"cache: wrote"*"app.d6c"* ]]
[[ "$out" == *$'\n'"42" ]]
[[ -f "$work/app.d6c" ]]
count=$((count + 1))

# --- second run, source unchanged: a hit, no re-write, same result ---
out="$(cd "$work" && DIAMOND_TRACE_CACHE=1 "$diamond" app.d6 2>&1)"
[[ "$out" == *"cache: hit"*"app.d6c"* ]]
[[ "$out" != *"cache: wrote"* ]]
[[ "$out" == *$'\n'"42" ]]
count=$((count + 1))

# --- editing only the required file (entry file untouched) still
# invalidates -- the cache key covers the whole expanded bundle, not
# just the one path named on the command line ---
sed -i.bak 's/n \* 2/n * 3/' "$work/lib.d6"
out="$(cd "$work" && DIAMOND_TRACE_CACHE=1 "$diamond" app.d6 2>&1)"
[[ "$out" == *"cache: miss"* ]]
[[ "$out" == *$'\n'"63" ]]
count=$((count + 1))

# --- a truncated/corrupted .d6c falls back to a clean recompile, never
# a crash, and never a wrong result ---
head -c 40 /dev/urandom > "$work/app.d6c"
out="$(cd "$work" && DIAMOND_TRACE_CACHE=1 "$diamond" app.d6 2>&1)"
[[ "$out" == *"cache: miss"* ]]
[[ "$out" == *$'\n'"63" ]]
count=$((count + 1))

: > "$work/app.d6c"
out="$(cd "$work" && DIAMOND_TRACE_CACHE=1 "$diamond" app.d6 2>&1)"
[[ "$out" == *"cache: miss"* ]]
[[ "$out" == *$'\n'"63" ]]
count=$((count + 1))

# --- a .d6c whose body was damaged after it was written (bit rot, a partial
# overwrite) is a miss, not a crash: the header still matches, so only the
# body checksum can notice ---
(cd "$work" && "$diamond" app.d6 >/dev/null)
size="$(stat -c %s "$work/app.d6c" 2>/dev/null || stat -f %z "$work/app.d6c")"
printf '\xff' | dd of="$work/app.d6c" bs=1 seek=$((size / 2)) conv=notrunc 2>/dev/null
out="$(cd "$work" && DIAMOND_TRACE_CACHE=1 "$diamond" app.d6 2>&1)"
[[ "$out" == *"cache: miss"* ]]
[[ "$out" == *$'\n'"63" ]]
count=$((count + 1))

# --- the checksum only catches accidents. Re-sign deliberately mutated
# bodies so the structural and bytecode validation has to hold on its own:
# every mutant must be rejected (a miss, clean recompile), run to a normal
# exit, or run on -- never die on a signal ---
cat > "$work/forge.d6" <<'EOF2'
class Animal
  def initialize(name: String)
    @name = name
  end
  def speak() = "#{@name} makes a sound"
end
class Dog < Animal
  def speak() = "#{@name} barks"
end
h = {"a": 1, "b": [1, 2, 3]}
total = 0
(0..9).each() do |i| total += i * 2 end
puts(Dog.new("rex").speak())
puts(h["b"].map() do |x| x + total end.to_s())
EOF2
(cd "$work" && "$diamond" forge.d6 >/dev/null)
forged_crashes="$(cd "$work" && "$diamond" "$repo_root/tests/forge_cache.d6" "$diamond" forge.d6 200)"
[[ "$forged_crashes" == "0" ]]
count=$((count + 1))

# --- DIAMOND_NO_CACHE=1 skips it entirely -- no trace output, no
# rewritten .d6c even though one already exists from the runs above ---
rm -f "$work/app.d6c"
out="$(cd "$work" && DIAMOND_NO_CACHE=1 DIAMOND_TRACE_CACHE=1 "$diamond" app.d6 2>&1)"
[[ "$out" != *"cache:"* ]]
[[ ! -f "$work/app.d6c" ]]
count=$((count + 1))

# --- -e never creates a cache file, anywhere, regardless of cwd ---
rm -f "$work"/*.d6c
(cd "$work" && DIAMOND_TRACE_CACHE=1 "$diamond" -e '1 + 1' >/dev/null 2>&1)
[[ -z "$(find "$work" -name '*.d6c' 2>/dev/null)" ]]
count=$((count + 1))

# --- string constants are stored compactly: 400 distinct string
# literals used to cost 4KB each (1.6MB on top of the prelude); the whole
# cache must stay a small fraction of that, and still load correctly ---
{
    printf 'words = []\n'
    for i in $(seq 1 400); do printf 'words.push("literal number %s")\n' "$i"; done
    printf 'words.length()\n'
} > "$work/strings.d6"
(cd "$work" && "$diamond" strings.d6 >/dev/null)
prelude_only="$(cd "$work" && printf '1\n' > tiny.d6 && "$diamond" tiny.d6 >/dev/null && stat -c %s tiny.d6c 2>/dev/null || stat -f %z tiny.d6c)"
with_strings="$(stat -c %s "$work/strings.d6c" 2>/dev/null || stat -f %z "$work/strings.d6c")"
(( with_strings - prelude_only < 400 * 1024 ))
[[ "$(cd "$work" && "$diamond" strings.d6)" == "400" ]]
count=$((count + 1))

# A full module table must survive serialization, including calls and
# includes referring to its final entry.
cp tests/cases/module_limit_boundary.d6 "$work/modules.d6"
out="$(cd "$work" && DIAMOND_TRACE_CACHE=1 "$diamond" modules.d6 2>&1)"
[[ "$out" == *"cache: wrote"* ]]
[[ "$out" == *$'\n'"[42, 42, 42, 42]" ]]
out="$(cd "$work" && DIAMOND_TRACE_CACHE=1 "$diamond" modules.d6 2>&1)"
[[ "$out" == *"cache: hit"* ]]
[[ "$out" == *$'\n'"[42, 42, 42, 42]" ]]
count=$((count + 1))

# --- a cache does not carry the prelude: the binary already embeds it, so a
# trivial script's file stays small (it was about 4 MB with the prelude), and
# the program still runs from the cache with identical output ---
printf 'puts(6 * 7)\n' > "$work/small.d6"
(cd "$work" && "$diamond" small.d6 >/dev/null)
small_size="$(stat -c %s "$work/small.d6c" 2>/dev/null || stat -f %z "$work/small.d6c")"
(( small_size < 200 * 1024 ))
out="$(cd "$work" && DIAMOND_TRACE_CACHE=1 "$diamond" small.d6 2>&1)"
[[ "$out" == *"cache: hit"* ]]
[[ "$out" == *$'\n'"42"$'\n'"nil" ]]
count=$((count + 1))

# --- a program that uses JSON is compiled against the same embedded prelude
# (which includes JSON), so its cache stays small too and still round-trips ---
printf 'puts(JSON.stringify({"a": [1, 2]}))\n' > "$work/json.d6"
first="$(cd "$work" && "$diamond" json.d6)"
json_size="$(stat -c %s "$work/json.d6c" 2>/dev/null || stat -f %z "$work/json.d6c")"
(( json_size < 200 * 1024 ))
out="$(cd "$work" && DIAMOND_TRACE_CACHE=1 "$diamond" json.d6 2>&1)"
[[ "$out" == *"cache: hit"* ]]
[[ "$out" == *"$first" ]]
count=$((count + 1))

# --- `diamond cache clear`: removes caches, only real ones, never follows links ---
mkdir -p "$work/clear/sub"
printf 'puts(1)\n' > "$work/clear/a.d6"
printf 'puts(2)\n' > "$work/clear/sub/b.d6"
(cd "$work/clear" && "$diamond" a.d6 >/dev/null && "$diamond" sub/b.d6 >/dev/null)
printf 'not a cache' > "$work/clear/unrelated.d6c"
[[ -f "$work/clear/a.d6c" && -f "$work/clear/sub/b.d6c" ]]

out="$("$diamond" cache clear --dry-run "$work/clear")"
[[ "$out" == *"would remove 2 cache files"* ]]
[[ -f "$work/clear/a.d6c" && -f "$work/clear/sub/b.d6c" ]]
count=$((count + 1))

out="$("$diamond" cache clear "$work/clear/a.d6")"
[[ "$out" == *"removed 1 cache file"* ]]
[[ ! -f "$work/clear/a.d6c" && -f "$work/clear/sub/b.d6c" ]]
count=$((count + 1))

out="$(cd "$work/clear" && "$diamond" cache clear)"
[[ "$out" == *"removed 1 cache file"* ]]
[[ ! -f "$work/clear/sub/b.d6c" ]]
[[ "$(cat "$work/clear/unrelated.d6c")" == "not a cache" ]]
count=$((count + 1))

# A script whose cache was cleared simply recompiles and rewrites it.
out="$(cd "$work/clear" && DIAMOND_TRACE_CACHE=1 "$diamond" a.d6 2>&1)"
[[ "$out" == *"cache: miss"* && "$out" == *"cache: wrote"* ]]
count=$((count + 1))

# --- a program that declares names of its own that collide with JSON's still
# compiles (it falls back to a live prelude compile) and caches correctly ---
printf 'class JSONError < StandardError\nend\nclass JSONCodec\n  def tag()\n    "mine"\n  end\nend\nputs(JSONCodec.new().tag())\n' > "$work/own_json.d6"
first="$(cd "$work" && "$diamond" own_json.d6)"
[[ "$first" == "mine"$'\n'"nil" ]]
out="$(cd "$work" && DIAMOND_TRACE_CACHE=1 "$diamond" own_json.d6 2>&1)"
[[ "$out" == *"cache: hit"* ]]
[[ "$out" == *"$first" ]]
count=$((count + 1))

# --- `diamond cache warm`: compile and cache without running ---
mkdir -p "$work/warm"
printf 'puts("ran")\n' > "$work/warm/app.d6"
out="$("$diamond" cache warm "$work/warm/app.d6")"
[[ "$out" == "cached "*"app.d6c" ]]
[[ -f "$work/warm/app.d6c" ]]
# It did not run the program, and a later run reads the warmed cache.
[[ "$out" != *"ran"* ]]
out="$(cd "$work/warm" && DIAMOND_TRACE_CACHE=1 "$diamond" app.d6 2>&1)"
[[ "$out" == *"cache: hit"* && "$out" == *"ran"* ]]
out="$("$diamond" cache warm "$work/warm/app.d6")"
[[ "$out" == "up to date "* ]]
count=$((count + 1))

# A program that does not compile is reported and leaves no cache behind.
printf 'puts(\n' > "$work/warm/bad.d6"
status=0
"$diamond" cache warm "$work/warm/bad.d6" >/dev/null 2>&1 || status=$?
[[ "$status" == 65 && ! -f "$work/warm/bad.d6c" ]]
count=$((count + 1))

# A read-only directory holding a warmed cache still reads it (skipped when the
# test runs as root, which can write anywhere).
if [[ "$(id -u)" != 0 ]]; then
    mkdir -p "$work/ro"
    printf 'puts("from ro")\n' > "$work/ro/app.d6"
    "$diamond" cache warm "$work/ro/app.d6" >/dev/null
    chmod a-w "$work/ro"
    out="$(cd "$work/ro" && DIAMOND_TRACE_CACHE=1 "$diamond" app.d6 2>&1)"
    [[ "$out" == *"cache: hit"* && "$out" == *"from ro"* ]]
    # Warming where nothing can be written says so instead of claiming success.
    chmod u+w "$work/ro"
    printf 'puts(1)\n' > "$work/ro/other.d6"
    chmod a-w "$work/ro"
    status=0
    "$diamond" cache warm "$work/ro/other.d6" >/dev/null 2>&1 || status=$?
    chmod u+w "$work/ro"
    [[ "$status" == 73 ]]
    count=$((count + 1))
fi

echo "$count cache tests passed"
