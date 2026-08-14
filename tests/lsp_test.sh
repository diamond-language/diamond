#!/usr/bin/env bash
set -euo pipefail

# diamond-lsp is driven over its real stdio transport (Content-Length-framed
# JSON-RPC, matching every mainstream editor's own language client) via a
# bash coproc -- no Python/Node dependency, matching every other test script
# in this repo. Assertions on message *content* are plain substring matches
# (grep-Fq-style, via bash [[ == *pattern* ]]), the same tolerance
# tests/parser_error_cases already relies on: exact key ordering isn't the
# contract, the presence of the right fields and values is.

diamond_lsp="$(realpath ./build/diamond-lsp)"
count=0

# A real on-disk directory for the require-resolution cases below --
# `require` only resolves against an actual directory, so unlike every
# other case in this script (which use a uri that was never a real path
# at all), these need one.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cat > "$work/helper.di" <<'EOF'
def greet(name)
  "hello, " + name
end
EOF

send() {
    local body="$1"
    printf 'Content-Length: %d\r\n\r\n%s' "${#body}" "$body" >&"${LSP[1]}"
}

read_message() {
    local line length=-1 body
    while IFS= read -r -u "${LSP[0]}" line; do
        line="${line%$'\r'}"
        [[ -z "$line" ]] && break
        if [[ "$line" == Content-Length:* ]]; then
            length="${line#Content-Length: }"
        fi
    done
    if (( length < 0 )); then
        echo "lsp_test: message with no Content-Length header" >&2
        exit 1
    fi
    IFS= read -r -u "${LSP[0]}" -N "$length" body
    printf '%s' "$body"
}

coproc LSP { "$diamond_lsp"; }

# --- initialize advertises full-document sync and hover ---

send '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
response="$(read_message)"
[[ "$response" == *'"id":1'* ]]
count=$((count + 1))
[[ "$response" == *'"textDocumentSync":1'* ]]
count=$((count + 1))
[[ "$response" == *'"hoverProvider":true'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"initialized","params":{}}'

# --- didOpen on broken source publishes exactly one diagnostic ---

send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///broken.di","text":"def f(\n"}}}'
response="$(read_message)"
[[ "$response" == *'"method":"textDocument/publishDiagnostics"'* ]]
count=$((count + 1))
[[ "$response" == *'"uri":"file:///broken.di"'* ]]
count=$((count + 1))
[[ "$response" == *'"message":"expected parameter name"'* ]]
count=$((count + 1))
[[ "$response" == *'"severity":1'* ]]
count=$((count + 1))

# --- didChange with a full-sync replacement clears the diagnostic ---

send '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///broken.di"},"contentChanges":[{"text":"puts(1 + 2)"}]}}'
response="$(read_message)"
[[ "$response" == *'"uri":"file:///broken.di"'* ]]
count=$((count + 1))
[[ "$response" == *'"diagnostics":[]'* ]]
count=$((count + 1))

# --- didClose also publishes an empty diagnostics array ---

send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///reopen.di","text":"def f(\n"}}}'
read_message >/dev/null
send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"file:///reopen.di"}}}'
response="$(read_message)"
[[ "$response" == *'"uri":"file:///reopen.di"'* ]]
count=$((count + 1))
[[ "$response" == *'"diagnostics":[]'* ]]
count=$((count + 1))

# --- a valid require against a real on-disk file: no false diagnostic ---

main_uri="file://$work/main.di"
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$main_uri"'","text":"require \"helper\"\nputs(greet(\"world\"))"}}}'
response="$(read_message)"
[[ "$response" == *"\"uri\":\"$main_uri\""* ]]
count=$((count + 1))
[[ "$response" == *'"diagnostics":[]'* ]]
count=$((count + 1))

# --- a real error on the line *after* a require reports the right line/message ---

send '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$main_uri"'"},"contentChanges":[{"text":"require \"helper\"\nputs(missing_function())"}]}}'
response="$(read_message)"
[[ "$response" == *'"message":"undefined function"'* ]]
count=$((count + 1))
[[ "$response" == *'"start":{"line":1,'* ]]
count=$((count + 1))

# --- an unresolvable require reports diamond_load_program's own error ---

send '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$main_uri"'"},"contentChanges":[{"text":"require \"nonexistent\""}]}}'
response="$(read_message)"
[[ "$response" == *"cannot require"* ]]
count=$((count + 1))
[[ "$response" == *"nonexistent"* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$main_uri"'"}}}'
read_message >/dev/null

# --- hover resolves a top-level function name, at its own declaration
# and at a bare call site, and a class name including its superclass ---

hover_uri="file://$work/hover.di"
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$hover_uri"'","text":"def add(a: Int, b: Int) -> Int\n  a + b\nend\n\nclass Base\nend\n\nclass Derived < Base\nend\n\nadd(1, 2)"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":4,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$hover_uri"'"},"position":{"line":10,"character":1}}}'
response="$(read_message)"
[[ "$response" == *'"id":4'* ]]
count=$((count + 1))
[[ "$response" == *'"value":"def add(a: Int, b: Int) -> Int"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":5,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$hover_uri"'"},"position":{"line":7,"character":8}}}'
response="$(read_message)"
[[ "$response" == *'"value":"class Derived < Base"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":6,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$hover_uri"'"},"position":{"line":7,"character":17}}}'
response="$(read_message)"
[[ "$response" == *'"value":"class Base"'* ]]
count=$((count + 1))

# --- hover on a local variable returns null: no symbol table for those,
# only top-level functions and classes (see docs/lsp.md) ---

send '{"jsonrpc":"2.0","id":7,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$hover_uri"'"},"position":{"line":1,"character":2}}}'
response="$(read_message)"
[[ "$response" == *'"id":7'* ]]
count=$((count + 1))
[[ "$response" == *'"result":null'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$hover_uri"'"}}}'
read_message >/dev/null

# --- hover on a document that doesn't currently compile returns null,
# not a stale/partial signature ---

broken_hover_uri="file:///hover_broken.di"
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$broken_hover_uri"'","text":"def f(\n"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":8,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$broken_hover_uri"'"},"position":{"line":0,"character":4}}}'
response="$(read_message)"
[[ "$response" == *'"result":null'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$broken_hover_uri"'"}}}'
read_message >/dev/null

# --- an unrecognized method gets a JSON-RPC MethodNotFound error ---

send '{"jsonrpc":"2.0","id":2,"method":"textDocument/definition","params":{}}'
response="$(read_message)"
[[ "$response" == *'"id":2'* ]]
count=$((count + 1))
[[ "$response" == *'"error":{"code":-32601'* ]]
count=$((count + 1))

# --- shutdown then exit: clean exit code 0 ---

send '{"jsonrpc":"2.0","id":3,"method":"shutdown"}'
response="$(read_message)"
[[ "$response" == *'"id":3'* ]]
count=$((count + 1))
[[ "$response" == *'"result":null'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"exit"}'
wait "$LSP_PID"
count=$((count + 1))

# --- exit without a prior shutdown: exit code 1, per the LSP spec ---

coproc LSP2 { "$diamond_lsp"; }
exit_only='{"jsonrpc":"2.0","method":"exit"}'
printf 'Content-Length: %d\r\n\r\n%s' "${#exit_only}" "$exit_only" >&"${LSP2[1]}"
if wait "$LSP2_PID"; then
    echo "lsp_test: exit without shutdown unexpectedly returned status 0" >&2
    exit 1
fi
count=$((count + 1))

echo "$count lsp tests passed"
