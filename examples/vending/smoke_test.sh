#!/usr/bin/env bash
# Runs vending through the interpreter and as a `diamond build` binary and
# checks a long scripted session (purchases, change, sold-out, rejected
# coins, refunds, wrong and right service keys, restocking), then a script
# of malformed commands whose errors go to stderr and set exit status 1.
# Set DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" build vending.di -o "$work/vending" > "$work/build.log"

status_of() { local s=0; "$@" > /dev/null 2>&1 || s=$?; echo "$s"; }

for vm in "$diamond vending.di" "$work/vending"; do
  $vm testdata/session.txt | cmp testdata/session.expected -
  [[ "$(status_of $vm testdata/session.txt)" == 0 ]]

  { $vm testdata/bad.txt 2> "$work/bad.err" || true; } | cmp testdata/bad.expected -
  [[ "$(status_of $vm testdata/bad.txt)" == 1 ]]
  grep -q "^2: not JSON" "$work/bad.err"
  grep -qx "3: unknown command 'dance'" "$work/bad.err"
  grep -qx '4: bad arguments for coin: {}' "$work/bad.err"
  grep -qx '6: bad arguments for coin: {"cents":25,"colour":"red"}' "$work/bad.err"
  grep -qx "7: refund takes no arguments" "$work/bad.err"
  grep -qx '8: expected an object with a "cmd" key' "$work/bad.err"

  [[ "$(status_of $vm testdata/missing.txt)" == 66 ]]
  [[ "$(status_of $vm)" == 64 ]]
  [[ "$(status_of $vm a b)" == 64 ]]
done

echo "vending smoke test passed"
