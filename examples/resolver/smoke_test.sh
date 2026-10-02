#!/usr/bin/env bash
# Runs resolver through the interpreter and as a `diamond build` binary over
# a fixed set of resolutions (newest-wins, pinned versions, a conflict the
# greedy walk cannot avoid and the pin ordering that avoids it, a missing
# package, a cycle, an unmatched constraint), the --check mode, every error
# path's exit status, and a script that exercises freezing and generic
# argument checking. Output is compared to testdata/session.expected.
# Set DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" build resolver.di -o "$work/resolver" > "$work/build.log"

# Prints "$ resolver args", the program's stdout, and its exit status for one
# run of the command held in VM (an array: either `diamond resolver.di` or the
# built binary), so both modes produce an identical transcript.
transcript() {
  local status=0
  echo "\$ resolver $*"
  "${VM[@]}" "$@" 2> /dev/null || status=$?
  echo "exit $status"
}

session() {
  local index=testdata/index.txt
  transcript "$index" 'web:>=1.0.0'
  transcript "$index" 'web:=1.1.0'
  transcript "$index" 'web:1.0.0..1.1.0'
  transcript "$index" 'admin:*'
  transcript "$index" 'json:=1.0.0' 'web:=1.0.0' 'admin:*'
  transcript "$index" 'rack:=2.0.0' 'web:=1.1.0'
  transcript "$index" 'ghost:*'
  transcript "$index" 'cycle_a:*'
  transcript "$index" 'web:>=3.0.0'
  transcript --check "$index"
  transcript --check testdata/dangling.txt
}

# Both binaries must print exactly the golden transcript.
VM=("$diamond" resolver.di)
session | cmp testdata/session.expected -
VM=("$work/resolver")
session | cmp testdata/session.expected -

# Error paths: usage (64), bad data (65), unreadable index (66). Each must
# also say something useful on stderr.
for vm in "$diamond resolver.di" "$work/resolver"; do
  status_of() { local s=0; $vm "$@" > /dev/null 2> "$work/err" || s=$?; echo "$s"; }
  [[ "$(status_of)" == 64 ]]
  [[ "$(status_of testdata/index.txt)" == 64 ]]
  [[ "$(status_of testdata/missing.txt web:'*')" == 66 ]]
  [[ "$(status_of testdata/bad_index.txt web:'*')" == 65 ]]
  grep -qx "bad dependency 'rack' in: web 1.0.0 rack" "$work/err"
  [[ "$(status_of testdata/bad_version.txt web:'*')" == 65 ]]
  grep -qx "line 2: not a version: x.y.z" "$work/err"
  [[ "$(status_of testdata/index.txt web)" == 65 ]]
  grep -qx "expected name:constraint, got 'web'" "$work/err"
  [[ "$(status_of testdata/index.txt web:'~1.0')" == 65 ]]
  grep -qx "not a constraint: ~1.0" "$work/err"
done

# Freezing and generic checks run inside the interpreter only.
"$diamond" testdata/frozen.di | cmp testdata/frozen.expected -

echo "resolver smoke test passed"
