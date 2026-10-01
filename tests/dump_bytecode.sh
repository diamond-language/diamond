#!/usr/bin/env bash
set -euo pipefail
diamond="${DIAMOND_BIN:-$PWD/build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
trap 'echo "dump-bytecode test failed at line $LINENO: $BASH_COMMAND" >&2' ERR

# Template-backed compilation and script arguments.
"$diamond" --dump-bytecode=user -e 'def app() = 42; puts(ARGV[0]); app()' hello > "$work/user"
grep -q '^== app ==$' "$work/user"
[[ "$(grep -c '^== ' "$work/user")" == 2 ]]
[[ "$(tail -2 "$work/user")" == $'hello\n42' ]]

# JSON uses a combined preamble/source compile, including top-level markers.
# Its built-in stringify body must disappear, while application methods stay.
source='def app() = JSON.stringify(42); class UserFormatter; def self.stringify() = app(); end; UserFormatter.stringify()'
"$diamond" --dump-bytecode=user -e "$source" > "$work/json-user"
"$diamond" --dump-bytecode=all -e "$source" > "$work/json-all"
grep -q '^== app ==$' "$work/json-user"
[[ "$(grep -c '^== ' "$work/json-user")" == 3 ]]
[[ "$(grep -c '^== stringify ==$' "$work/json-user")" == 1 ]]
grep -q '^== stringify ==$' "$work/json-all"
# Every displayed instruction retains its real offset/operands from the full dump.
while IFS= read -r instruction; do
    grep -Fxq "$instruction" "$work/json-all"
done < <(grep '^[0-9]' "$work/json-user")

# Imported application code is included; warm caches cannot reintroduce preamble.
cat > "$work/helper.di" <<'DI'
def imported() = 40
class Box
  def value() = 2
end
DI
cat > "$work/main.di" <<'DI'
require "helper"
def app() = imported() + Box.new().value()
puts(ARGV[0])
app()
DI
env -u DIAMOND_NO_CACHE "$diamond" --dump-bytecode "$work/main.di" hello > "$work/all"
env -u DIAMOND_NO_CACHE DIAMOND_TRACE_CACHE=1 "$diamond" --dump-bytecode=all "$work/main.di" hello > "$work/all-alias" 2> "$work/cache-log"
grep -q 'cache: hit' "$work/cache-log"
cp "$work/main.dic" "$work/cache-before"
cmp "$work/all" "$work/all-alias"
env -u DIAMOND_NO_CACHE "$diamond" --dump-bytecode=user "$work/main.di" hello > "$work/file-user"
env -u DIAMOND_NO_CACHE "$diamond" --dump-bytecode=user "$work/main.di" hello > "$work/file-user-again"
cmp "$work/file-user" "$work/file-user-again"
cmp "$work/main.dic" "$work/cache-before"
for name in imported value app; do grep -q "^== $name ==$" "$work/file-user"; done
[[ "$(grep -c '^== ' "$work/file-user")" == 4 ]]
[[ "$(tail -2 "$work/file-user")" == $'hello\n42' ]]

# Debug instrumentation uses the combined compile path even without JSON.
DIAMOND_DEBUG_BREAKPOINTS='' "$diamond" --dump-bytecode=user -e 'def app() = 42; app()' > "$work/debug"
[[ "$(grep -c '^== ' "$work/debug")" == 2 ]]
[[ "$(tail -1 "$work/debug")" == 42 ]]

status=0
"$diamond" --dump-bytecode=bogus -e 1 >/dev/null 2>&1 || status=$?
[[ "$status" == 64 ]]
echo 'targeted bytecode dump tests passed'
