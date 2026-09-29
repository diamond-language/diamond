#!/usr/bin/env bash
# Runs the brainfuck interpreter and depth.di through the interpreter and
# as `diamond build` binaries. hello.bf (906 instructions) and busy.bf
# (326765) both run in a single call frame, far past the ordinary depth
# limit of 95; depth.di shows which recursion shapes get that treatment.
# Set DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" build brainfuck.di -o "$work/brainfuck" > "$work/build.log"
"$diamond" build depth.di -o "$work/depth" >> "$work/build.log"

status_of() { local s=0; "$@" > /dev/null 2>&1 || s=$?; echo "$s"; }
message_of() { "$@" 2>&1 >/dev/null || true; }

for bf in "$diamond brainfuck.di" "$work/brainfuck"; do
  [[ "$($bf testdata/hello.bf)" == "Hello World!" ]]
  [[ "$(message_of $bf testdata/hello.bf --steps)" == "906 instructions" ]]
  [[ "$($bf testdata/cat.bf --input 'hi there')" == "hi there" ]]
  [[ "$($bf testdata/cat.bf)" == "" ]]
  [[ "$($bf testdata/busy.bf)" == "A" ]]
  [[ "$(message_of $bf testdata/busy.bf --steps)" == "326765 instructions" ]]

  # A program that never ends is stopped by the step limit; without one, a
  # self-recursive tail call would simply run forever.
  [[ "$(message_of $bf testdata/forever.bf --limit 20000)" == "step limit of 20000 exceeded (at instruction 2)" ]]
  [[ "$(status_of $bf testdata/forever.bf --limit 20000)" == 65 ]]
  [[ "$(message_of $bf testdata/unmatched_open.bf)" == "unmatched [ at instruction 0" ]]
  [[ "$(message_of $bf testdata/unmatched_close.bf)" == "unmatched ] at instruction 1" ]]
  [[ "$(message_of $bf testdata/left.bf)" == "pointer ran off the left end of the tape at instruction 0" ]]
  [[ "$(status_of $bf testdata/left.bf)" == 65 ]]
  [[ "$(status_of $bf testdata/missing.bf)" == 66 ]]
  [[ "$(status_of $bf)" == 64 ]]
  [[ "$(status_of $bf testdata/hello.bf --limit 0)" == 64 ]]
  [[ "$(status_of $bf testdata/hello.bf --input)" == 64 ]]
done

for depth in "$diamond depth.di" "$work/depth"; do
  $depth | cmp testdata/depth.expected -
  [[ "$($depth 50 | grep -c 'stack overflow')" == 0 ]]
  [[ "$(status_of $depth 0)" == 64 ]]
done

echo "brainfuck smoke test passed"
