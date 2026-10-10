#!/usr/bin/env bash
set -euo pipefail
trace="$(mktemp)"
trap 'rm -f "$trace"' EXIT
if ! DIAMOND_TRACE_COMPILE=1 ./build/analysis_cache_test 2>"$trace"; then
    cat "$trace" >&2
    exit 1
fi
# Seven successful compiles: initial, open import, closed import, disk edit,
# root edit, recovery after failure, and recovery after clearing. Cache hits
# must not invoke the compiler; the syntax error exits before emitting a trace.
compiles="$(grep -c '^compile: discovery ' "$trace")"
if [[ "$compiles" != 7 ]]; then
    cat "$trace" >&2
    echo "analysis cache: expected 7 compiles, got $compiles" >&2
    exit 1
fi
echo 'analysis cache reuse and invalidation tests passed'

# Exercise sharing between real request handlers, beyond the cache API tests.
send() {
    local body="$1"
    printf 'Content-Length: %d\r\n\r\n%s' "${#body}" "$body"
}
output="$({
    send '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
    send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"untitled:cache","text":"class CachePet\n  def bark() = 1\nend\ndef inspect_pet(pet: CachePet)\n  pet.bark()\nend\n"}}}'
    send '{"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"untitled:cache"},"position":{"line":4,"character":6}}}'
    send '{"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"untitled:cache"},"position":{"line":4,"character":7}}}'
    send '{"jsonrpc":"2.0","id":4,"method":"textDocument/definition","params":{"textDocument":{"uri":"untitled:cache"},"position":{"line":4,"character":7}}}'
    send '{"jsonrpc":"2.0","id":5,"method":"textDocument/completion","params":{"textDocument":{"uri":"untitled:cache"},"position":{"line":4,"character":6}}}'
    send '{"jsonrpc":"2.0","id":6,"method":"shutdown","params":{}}'
    send '{"jsonrpc":"2.0","method":"exit","params":{}}'
} | DIAMOND_TRACE_COMPILE=1 ./build/diamond-lsp 2>"$trace")"
[[ "$output" == *'"id":2,"result":['* ]]
[[ "$output" == *'"label":"bark","kind":3'* ]]
[[ "$output" == *'"id":3,"result":{"contents":'* ]]
[[ "$output" == *'"id":4,"result":{"uri":"untitled:cache"'* ]]
[[ "$output" == *'"id":5,"result":['* ]]
compiles="$(grep -c '^compile: discovery ' "$trace")"
if [[ "$compiles" != 2 ]]; then
    cat "$trace" >&2
    echo "analysis cache: expected diagnostics and one shared analysis, got $compiles compiles" >&2
    exit 1
fi
echo 'completion, hover, and definition share one analysis'
