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

# --- a .dic whose body was damaged after it was written (bit rot, a partial
# overwrite) is a miss, not a crash: the header still matches, so only the
# body checksum can notice ---
(cd "$work" && "$diamond" app.di >/dev/null)
size="$(stat -c %s "$work/app.dic" 2>/dev/null || stat -f %z "$work/app.dic")"
printf '\xff' | dd of="$work/app.dic" bs=1 seek=$((size / 2)) conv=notrunc 2>/dev/null
out="$(cd "$work" && DIAMOND_TRACE_CACHE=1 "$diamond" app.di 2>&1)"
[[ "$out" == *"cache: miss"* ]]
[[ "$out" == *$'\n'"63" ]]
count=$((count + 1))

# --- the checksum only catches accidents. Re-sign deliberately mutated
# bodies so the structural and bytecode validation has to hold on its own:
# every mutant must be rejected (a miss, clean recompile), run to a normal
# exit, or run on -- never die on a signal ---
if command -v python3 >/dev/null; then
    cat > "$work/forge.di" <<'EOF2'
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
    (cd "$work" && "$diamond" forge.di >/dev/null)
    forged_crashes="$(python3 - "$work" "$diamond" <<'EOF2'
import os, random, subprocess, sys
work, diamond = sys.argv[1], os.path.abspath(sys.argv[2])
base = open(os.path.join(work, "forge.dic"), "rb").read()
MASK = (1 << 64) - 1
def checksum(body):
    h = (0x9e3779b97f4a7c15 ^ len(body)) & MASK
    i = 0
    while i + 8 <= len(body):
        h = ((h ^ int.from_bytes(body[i:i + 8], "little")) * 0xff51afd7ed558ccd) & MASK
        h ^= h >> 32
        i += 8
    tail = int.from_bytes(body[i:].ljust(8, b"\0"), "little")
    h = ((h ^ tail) * 0xff51afd7ed558ccd) & MASK
    return h ^ (h >> 29)
random.seed(20261004)
crashes = 0
for _ in range(200):
    data = bytearray(base)
    for _ in range(random.choice([1, 1, 2, 4])):
        at = random.randrange(100, 20000)
        data[at] = random.randrange(256) if random.random() < .5 else data[at] ^ (1 << random.randrange(8))
    data[80:88] = checksum(bytes(data[88:])).to_bytes(8, "little")
    open(os.path.join(work, "forge.dic"), "wb").write(data)
    try:
        status = subprocess.run([diamond, "forge.di"], cwd=work, capture_output=True, timeout=5).returncode
    except subprocess.TimeoutExpired:
        status = 0  # a mutant may legitimately loop forever; only a signal is a failure
    if status < 0:
        crashes += 1
print(crashes)
EOF2
)"
    [[ "$forged_crashes" == "0" ]]
    count=$((count + 1))
fi

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

# --- string constants are stored compactly: 400 distinct string
# literals used to cost 4KB each (1.6MB on top of the prelude); the whole
# cache must stay a small fraction of that, and still load correctly ---
{
    printf 'words = []\n'
    for i in $(seq 1 400); do printf 'words.push("literal number %s")\n' "$i"; done
    printf 'words.length()\n'
} > "$work/strings.di"
(cd "$work" && "$diamond" strings.di >/dev/null)
prelude_only="$(cd "$work" && printf '1\n' > tiny.di && "$diamond" tiny.di >/dev/null && stat -c %s tiny.dic 2>/dev/null || stat -f %z tiny.dic)"
with_strings="$(stat -c %s "$work/strings.dic" 2>/dev/null || stat -f %z "$work/strings.dic")"
(( with_strings - prelude_only < 400 * 1024 ))
[[ "$(cd "$work" && "$diamond" strings.di)" == "400" ]]
count=$((count + 1))

echo "$count cache tests passed"
