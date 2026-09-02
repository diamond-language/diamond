#!/usr/bin/env bash
set -euo pipefail

# exit() terminates the whole OS process immediately (see DIAMOND_OP_EXIT,
# src/vm.c's exit_helper) -- it can never be exercised from tests/cases/*.di,
# since build/run_cases runs its entire ~1000-case corpus in one shared
# process (tests/run_cases.c) and a real exit() there would kill the whole
# batch, not just this one case. Every assertion here goes through a real
# `diamond -e ...` subprocess instead, checking $? directly.
#
# `set -e` treats `var=$(cmd)` as a failing statement whenever cmd exits
# nonzero (which is the whole point here), so every such call below is
# wrapped in its own set +e / set -e pair rather than relying on the
# script-wide errexit.

diamond="$(realpath ./build/diamond)"
count=0

# --- exit(code) sets the process exit status, and halts before any
# later statement runs ---
set +e
out="$("$diamond" -e 'puts("before"); exit(5); puts("after")')"
status=$?
set -e
[[ "$status" == 5 ]]
[[ "$out" == "before" ]]
count=$((count + 1))

# --- exit() with no argument defaults to 0 ---
"$diamond" -e 'exit()' >/dev/null
count=$((count + 1))

# --- a rescued out-of-range code raises an ordinary, catchable
# ArgumentError -- it does NOT terminate the process; only a valid code
# (0..255) actually calls the real, unrescuable exit() ---
set +e
out="$("$diamond" -e '
begin
  exit(300)
rescue error: ArgumentError
  puts("caught: #{error.message()}")
end
exit(7)
')"
status=$?
set -e
[[ "$status" == 7 ]]
[[ "$out" == "caught: exit code must be between 0 and 255" ]]
count=$((count + 1))

# --- an uncaught out-of-range code still exits nonzero (an ordinary
# uncaught exception), distinct from the process-exit-code-7 case above ---
set +e
"$diamond" -e 'exit(300)' >/dev/null 2>/dev/null
status=$?
set -e
[[ "$status" != 0 ]]
[[ "$status" != 300 ]]
count=$((count + 1))

# --- a non-Int code raises TypeError (an uncaught exception, printed
# as "runtime error: <message>" -- an uncaught exception's own class
# name never appears in that text, only its message, so this checks
# exit_helper's actual message instead of the exception's class name) ---
set +e
error_output="$("$diamond" -e 'exit("nope")' 2>&1)"
status=$?
set -e
[[ "$status" != 0 ]]
[[ "$error_output" == *"exit code must be an Int"* ]]
count=$((count + 1))

echo "$count exit tests passed"
