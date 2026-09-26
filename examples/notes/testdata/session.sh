#!/usr/bin/env bash
# A scripted session: every command's output, in order. Run by
# smoke_test.sh with $notes set to the program and $NOTES_DB to a fresh
# file; its output must match session.expected.
set -uo pipefail
run() { echo "\$ notes $*"; "$notes" "$@" 2>&1; echo "[exit $?]"; }
body() { printf '%s\n' "$1" > "$work/body.txt"; }

body "Pick up flour, eggs, and the good butter."
NOTES_NOW=2026-08-14 run add Groceries home errands < "$work/body.txt"
printf 'Standup notes.\nThe deploy is blocked on the flaky gremlin test.\nFollow up with the database team.\n' > "$work/body.txt"
NOTES_NOW=2026-09-02 run add Standup work < "$work/body.txt"
body "Butter the pan before the eggs go in."
NOTES_NOW=2026-09-20 run add Omelette cooking home < "$work/body.txt"
run list
run list home
run show 2
run search butter
run search '"flaky gremlin"'
run tag 2 Ops
run untag 1 errands
run import testdata/import.json
run import testdata/bad_import.json
run list
run search 'brake OR websockets'
run rm 5
run search brake
run stats
run export
run show 99
run show abc
run untag 1 nope
run search AND
run import testdata/missing.json
run frobnicate
