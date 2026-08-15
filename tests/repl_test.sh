#!/usr/bin/env bash
set -euo pipefail

# The REPL is driven over plain pipes via a bash coproc -- no Python/Node
# dependency, matching every other test script in this repo.
# DIAMOND_FORCE_REPL bypasses the isatty() check that normally gates
# interactive-mode launch, since a coproc's stdin is a pipe, not a real
# pty (see main.c). Output has no message framing (unlike diamond-lsp's
# JSON-RPC), so each read waits for the REPL's own ">>> "/"... " prompt to
# reappear, byte by byte, rather than a length-prefixed body.

diamond="$(realpath ./build/diamond)"
count=0

read_until_prompt() {
    local chunk buffer=""
    while IFS= read -r -u "${REPL[0]}" -N 1 -t 10 chunk; do
        buffer+="$chunk"
        if [[ "$buffer" == *$'\n>>> ' || "$buffer" == ">>> " ||
              "$buffer" == *$'\n... ' || "$buffer" == "... " ]]; then
            break
        fi
    done
    printf '%s' "$buffer"
}

export DIAMOND_FORCE_REPL=1
coproc REPL { "$diamond"; }

banner="$(read_until_prompt)"
[[ "$banner" == *">>> " ]]
count=$((count + 1))

# --- a bare expression evaluates and prints its value ---

printf '1 + 2\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == $'3\n>>> ' ]]
count=$((count + 1))

# --- assignments persist across later evaluations ---

printf 'x = 10\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == $'10\n>>> ' ]]
count=$((count + 1))

printf 'x + 5\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == $'15\n>>> ' ]]
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
[[ "$response" == *">>> " ]]
count=$((count + 1))

printf 'add(3, 4)\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == $'7\n>>> ' ]]
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
[[ "$response" == *">>> " ]]
count=$((count + 1))

printf 'Box.new(42).get()\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == $'42\n>>> ' ]]
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
[[ "$response" == $'3\n>>> ' ]]
count=$((count + 1))

# --- a genuine syntax error (not just an expression that might continue
# on the next line) is reported and doesn't hang waiting for more input;
# the session recovers cleanly afterward ---

printf '1 + )\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == *"error:"* ]]
count=$((count + 1))
[[ "$response" == *">>> " ]]
count=$((count + 1))

printf '99\n' >&"${REPL[1]}"
response="$(read_until_prompt)"
[[ "$response" == $'99\n>>> ' ]]
count=$((count + 1))

# --- EOF (Ctrl-D) exits cleanly ---

exec {REPL[1]}>&-
wait "$REPL_PID" 2>/dev/null || true

echo "$count repl tests passed"
