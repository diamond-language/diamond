#!/usr/bin/env bash
# Runs tictactoe through the interpreter and as a `diamond build` binary,
# and checks a full self-play game (perfect play must draw), --best on a
# forced win, a forced block, and an empty board, and every error path.
# Set DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" tictactoe.di --self | cmp testdata/self.expected -
"$diamond" tictactoe.di --best "XX......." | grep -qx "o plays 2 -> o loses with best play"
"$diamond" tictactoe.di --best "........." | grep -qx "x plays 0 -> draw with best play"

"$diamond" build tictactoe.di -o "$work/tictactoe" > "$work/build.log"
ttt="$work/tictactoe"
"$ttt" --self | cmp testdata/self.expected -
"$ttt" --best "XX......." | grep -qx "o plays 2 -> o loses with best play"

status=0; "$ttt" --best "XXXOO...." 2> "$work/over.err" > /dev/null || status=$?
[[ "$status" == 65 ]] && grep -q "already over" "$work/over.err"

status=0; "$ttt" --best "XX" 2> "$work/short.err" > /dev/null || status=$?
[[ "$status" == 65 ]] && grep -q "exactly 9 characters" "$work/short.err"

status=0; "$ttt" --best "XXXXXXXXY" 2> "$work/bad.err" > /dev/null || status=$?
[[ "$status" == 65 ]] && grep -q "unexpected 'Y' at position 8" "$work/bad.err"

status=0; "$ttt" 2> /dev/null || status=$?
[[ "$status" == 64 ]]
status=0; "$ttt" --bogus 2> /dev/null || status=$?
[[ "$status" == 64 ]]

echo "tictactoe smoke test passed"
