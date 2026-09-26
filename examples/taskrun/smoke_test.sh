#!/usr/bin/env bash
# Runs taskrun against testdata/Taskfile (interpreted and as a
# `diamond build` binary) and checks ordering, failure handling, error
# exits, parallelism, and a child that writes more than a pipe buffer.
# Set DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" taskrun.di -f testdata/Taskfile all | diff -u testdata/all.expected -
"$diamond" build taskrun.di -o "$work/taskrun" > "$work/build.log"
run() { "$work/taskrun" -f testdata/Taskfile "$@"; }
run all | diff -u testdata/all.expected -

# A failing task: its dependents and anything not yet started are skipped.
status=0; run after-broken docs > "$work/broken.txt" || status=$?
[[ "$status" == 1 ]]
diff -u testdata/broken.expected "$work/broken.txt"

run --plan package | diff -u - <(printf '1. clean\n2. compile\n3. assets\n4. package\n')

status=0; run loop-a 2> "$work/err" || status=$?
[[ "$status" == 2 ]] && grep -q "dependency cycle: loop-a -> loop-b -> loop-c -> loop-a" "$work/err"
status=0; run nope 2> "$work/err" || status=$?
[[ "$status" == 2 ]] && grep -q "no task named 'nope'" "$work/err"
status=0; run 2> /dev/null || status=$?; [[ "$status" == 64 ]]
status=0; "$work/taskrun" -f "$work/missing" all 2> /dev/null || status=$?; [[ "$status" == 66 ]]

# Three independent 0.4s tasks: -j 3 finishes well before the 1.2s -j 1 takes.
printf 'a:\n  sleep 0.4\nb:\n  sleep 0.4\nc:\n  sleep 0.4\nall: a b c\n' > "$work/Parallel"
start=$(date +%s%N)
"$work/taskrun" -f "$work/Parallel" -j 3 all > /dev/null
elapsed_ms=$(( ($(date +%s%N) - start) / 1000000 ))
(( elapsed_ms < 1000 )) || { echo "-j 3 took ${elapsed_ms}ms" >&2; exit 1; }

# 200 KB of output is several pipe buffers; draining with IO.poll while the
# child runs keeps it from blocking (a bare wait() would deadlock).
printf 'chatty:\n  head -c 200000 /dev/zero | tr "\\\\0" x\n' > "$work/Chatty"
timeout 20 "$work/taskrun" -f "$work/Chatty" chatty > "$work/chatty.txt"
awk '$1 ~ /^x+$/ && length($1) == 200000 { found = 1 } END { exit !found }' "$work/chatty.txt"
echo "taskrun smoke test passed"
