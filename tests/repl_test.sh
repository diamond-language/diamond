#!/usr/bin/env bash
set -euo pipefail

# The REPL is driven over plain pipes via a bash coproc -- no Python/Node
# dependency, matching every other test script in this repo.
# DIAMOND_FORCE_REPL bypasses the isatty() check that normally gates
# interactive-mode launch, since a coproc's stdin is a pipe, not a real
# pty (see main.c). Output has no message framing (unlike diamond-lsp's
# JSON-RPC), so each read waits for the REPL's own "{> "/"... " prompt to
# reappear, byte by byte, rather than a length-prefixed body.

diamond="$(realpath ./build/diamond)"
count=0

read_until_prompt() {
    local chunk buffer=""
    while IFS= read -r -u "${REPL[0]}" -N 1 -t 10 chunk; do
        buffer+="$chunk"
        if [[ "$buffer" == *$'\n{> ' || "$buffer" == "{> " ||
              "$buffer" == *$'\n... ' || "$buffer" == "... " ]]; then
            break
        fi
    done
    printf '%s' "$buffer"
}

export DIAMOND_FORCE_REPL=1
coproc REPL { "$diamond"; }

banner="$(read_until_prompt)"
[[ "$banner" == *"{> " ]]
count=$((count + 1))

# --- a bare expression evaluates and prints its value ---

printf '1 + 2\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == $'3\n{> ' ]]
count=$((count + 1))

printf '_\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == $'3\n{> ' ]]
count=$((count + 1))

# --- assignments persist across later evaluations ---

printf 'x = 10\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == $'10\n{> ' ]]
count=$((count + 1))

printf 'x + 5\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == $'15\n{> ' ]]
count=$((count + 1))

# Heap-backed results must remain printable after the candidate VM returns.
# This used to trigger a use-after-free under ThreadSanitizer because the
# REPL freed the VM before printing the returned Range object.
printf '1..10\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == $'#<Range>\n{> ' ]]
count=$((count + 1))

printf '_\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == $'#<Range>\n{> ' ]]
count=$((count + 1))

# --- puts output shows once, not replayed on later rounds ---

printf 'puts("hello")\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == *"hello"* ]]
count=$((count + 1))

printf 'y = 1\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" != *"hello"* ]]
count=$((count + 1))

# --- multi-line function definitions, then calling the defined function ---

printf 'def add(a, b)\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == "... " ]]
count=$((count + 1))

printf '  a + b\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == "... " ]]
count=$((count + 1))

printf 'end\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == *"{> " ]]
count=$((count + 1))

printf 'add(3, 4)\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == $'7\n{> ' ]]
count=$((count + 1))

# Top-level methods can be replaced interactively.
for method_line in 'def add(a, b)' '  a - b'; do
    printf '%s\n' "$method_line" >&"${REPL[1]}"
    response="$(read_until_prompt)"
    [[ "$response" == "... " ]]
    count=$((count + 1))
done
printf 'end\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == *"{> " ]]
count=$((count + 1))
printf 'add(7, 3)\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == $'4\n{> ' ]]
count=$((count + 1))

# --- a class definition persists, instances keep their own state ---

for class_line in 'class Box' '  def initialize(v)' '    @v = v' '  end' \
                   '  def get()' '    @v' '  end'; do
    printf '%s\n' "$class_line" >&"${REPL[1]}"
    response="$(read_until_prompt)"
    [[ "$response" == "... " ]]
    count=$((count + 1))
done
printf 'end\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == *"{> " ]]
count=$((count + 1))

printf 'Box.new(42).get()\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == $'42\n{> ' ]]
count=$((count + 1))

# --- a trailing binary operator prompts for more input rather than
# erroring, and completing it on a later line evaluates the whole
# expression -- the compiler itself accepts a binary expression split
# across a line break (see docs/roadmap.md), and the REPL's own
# incomplete-vs-error heuristic (try_compile's *out_incomplete) correctly
# recognizes this as "needs more input", not a genuine error ---

printf '1 +\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == "... " ]]
count=$((count + 1))

printf '2\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == $'3\n{> ' ]]
count=$((count + 1))

# --- a genuine syntax error (not just an expression that might continue
# on the next line) is reported and doesn't hang waiting for more input;
# the session recovers cleanly afterward ---

printf '1 + )\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == *"error:"* ]]
count=$((count + 1))
[[ "$response" == *"{> " ]]
count=$((count + 1))

printf '99\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == $'99\n{> ' ]]
count=$((count + 1))

# --- EOF (Ctrl-D) exits cleanly ---

exec {REPL[1]}>&-
wait "$REPL_PID" 2>/dev/null || true

# --- a bare `exit`/`quit` (irb/pry/python-REPL convention) exits too,
# not just Ctrl-D -- each gets its own fresh coproc, since exiting ends
# the process the rest of this file's tests would otherwise keep reusing.
# Waited for with a real timeout (not just kill -0 once) so a regression
# that hangs instead of exiting fails the test rather than racing it. ---

assert_exit_word_exits() {
    local word="$1"
    coproc EXIT_REPL { DIAMOND_FORCE_REPL=1 "$diamond"; }
    local repl_pid="$EXIT_REPL_PID"
    local chunk buffer=
    while IFS= read -r -u "${EXIT_REPL[0]}" -N 1 -t 10 chunk; do
        buffer+="$chunk"
        [[ "$buffer" == *$'\n{> ' || "$buffer" == "{> " ]] && break
    done
    printf '%s\n' "$word" >&"${EXIT_REPL[1]}"
    exec {EXIT_REPL[1]}>&-
    local waited=0
    while kill -0 "$repl_pid" 2>/dev/null; do
        sleep 0.1
        waited=$((waited + 1))
        if [[ "$waited" -ge 50 ]]; then
            echo "process did not exit after '$word'" >&2
            kill -9 "$repl_pid" 2>/dev/null || true
            exit 1
        fi
    done
    wait "$repl_pid" 2>/dev/null || true
}
assert_exit_word_exits "exit"
count=$((count + 1))
assert_exit_word_exits "quit"
count=$((count + 1))

# `exit` appearing inside an in-progress multi-line block (not as the
# first line of a fresh statement) must not trigger early exit -- it's
# just the token `exit`, an undefined local, same as any other name.
coproc EXIT_REPL { DIAMOND_FORCE_REPL=1 "$diamond"; }
repl_pid="$EXIT_REPL_PID"
buffer=
while IFS= read -r -u "${EXIT_REPL[0]}" -N 1 -t 10 chunk; do
    buffer+="$chunk"
    [[ "$buffer" == *$'\n{> ' || "$buffer" == "{> " ]] && break
done
printf '1 +\n' >&"${EXIT_REPL[1]}"
buffer=
while IFS= read -r -u "${EXIT_REPL[0]}" -N 1 -t 10 chunk; do
    buffer+="$chunk"
    [[ "$buffer" == *$'\n... ' || "$buffer" == "... " ]] && break
done
printf 'exit\n' >&"${EXIT_REPL[1]}"
sleep 0.3
kill -0 "$repl_pid" 2>/dev/null
count=$((count + 1))
kill -9 "$repl_pid" 2>/dev/null || true
wait "$repl_pid" 2>/dev/null || true

echo "$count repl tests passed"
