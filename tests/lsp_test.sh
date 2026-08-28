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

# --- initialize advertises full-document sync, hover, and go-to-definition ---

send '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"workspaceFolders":[{"uri":"file://'"$work"'","name":"work"}]}}'
response="$(read_message)"
[[ "$response" == *'"id":1'* ]]
count=$((count + 1))
[[ "$response" == *'"textDocumentSync":1'* ]]
count=$((count + 1))
[[ "$response" == *'"hoverProvider":true'* ]]
count=$((count + 1))
[[ "$response" == *'"definitionProvider":true'* ]]
count=$((count + 1))
[[ "$response" == *'"documentSymbolProvider":true'* ]]
count=$((count + 1))
[[ "$response" == *'"completionProvider":{}'* ]]
count=$((count + 1))
[[ "$response" == *'"workspaceSymbolProvider":true'* ]]
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

# --- a real error *inside* a required file publishes a second
# notification against that file's own uri, not the requesting
# document's -- the requesting document's own publish (already read
# above, for the previous case) stays correctly empty, since its own
# text has no error ---

cat > "$work/broken_dependency.di" <<'EOF'
def broken(
  1
end
EOF
send '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$main_uri"'"},"contentChanges":[{"text":"require \"broken_dependency\"\nputs(1)"}]}}'
response="$(read_message)"
[[ "$response" == *"\"uri\":\"$main_uri\""* ]]
count=$((count + 1))
[[ "$response" == *'"diagnostics":[]'* ]]
count=$((count + 1))
response="$(read_message)"
[[ "$response" == *"\"uri\":\"file://$work/broken_dependency.di\""* ]]
count=$((count + 1))
[[ "$response" == *'"message":"expected parameter name"'* ]]
count=$((count + 1))

# --- an unresolvable require reports diamond_load_program's own error ---

send '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$main_uri"'"},"contentChanges":[{"text":"require \"nonexistent\""}]}}'
response="$(read_message)"
[[ "$response" == *"cannot require"* ]]
count=$((count + 1))
[[ "$response" == *"nonexistent"* ]]
count=$((count + 1))

# --- editing an open dependency's live buffer, unsaved, is seen
# immediately by a document that requires it, and re-publishes that
# document's diagnostics too -- not just once helper.di is saved to
# disk. Proves both live in-memory require resolution
# (document_resolve_source, lsp/document.h) and the dependency-cascade
# republish (lsp/dependencies.h) in one scenario: greet_loudly doesn't
# exist in helper.di on disk, so main.di referencing it starts out with
# a real "undefined function" error; adding greet_loudly to helper.di's
# *live* buffer (never written to disk) makes that error disappear from
# main.di's republished diagnostics without any didSave. ---

helper_uri="file://$work/helper.di"
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$helper_uri"'","text":"def greet(name)\n  \"hello, \" + name\nend"}}}'
response="$(read_message)"
[[ "$response" == *"\"uri\":\"$helper_uri\""* ]]
count=$((count + 1))
[[ "$response" == *'"diagnostics":[]'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$main_uri"'"},"contentChanges":[{"text":"require \"helper\"\nputs(greet_loudly(\"world\"))"}]}}'
response="$(read_message)"
[[ "$response" == *"\"uri\":\"$main_uri\""* ]]
count=$((count + 1))
[[ "$response" == *'"message":"undefined function"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$helper_uri"'"},"contentChanges":[{"text":"def greet(name)\n  \"hello, \" + name\nend\ndef greet_loudly(name)\n  greet(name) + \"!\"\nend"}]}}'
response="$(read_message)"
[[ "$response" == *"\"uri\":\"$helper_uri\""* ]]
count=$((count + 1))
[[ "$response" == *'"diagnostics":[]'* ]]
count=$((count + 1))
response="$(read_message)"
[[ "$response" == *"\"uri\":\"$main_uri\""* ]]
count=$((count + 1))
[[ "$response" == *'"diagnostics":[]'* ]]
count=$((count + 1))

# on-disk helper.di was never touched -- confirms this really came from
# the live buffer, not a save
[[ "$(cat "$work/helper.di")" != *"greet_loudly"* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$helper_uri"'"}}}'
read_message >/dev/null

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

# --- hover on a typed local exposes its position-sensitive structural fact ---

send '{"jsonrpc":"2.0","id":7,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$hover_uri"'"},"position":{"line":1,"character":2}}}'
response="$(read_message)"
[[ "$response" == *'"id":7'* ]]
count=$((count + 1))
[[ "$response" == *'"value":"Int"'* ]]
count=$((count + 1))

# --- go-to-definition resolves the same declaration kinds, to a
# Location inside the same document (declaration is a self-round-trip:
# resolving "Derived" at its own name lands right back on itself) ---

send '{"jsonrpc":"2.0","id":9,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$hover_uri"'"},"position":{"line":10,"character":1}}}'
response="$(read_message)"
[[ "$response" == *'"id":9'* ]]
count=$((count + 1))
[[ "$response" == *"\"uri\":\"$hover_uri\""* ]]
count=$((count + 1))
[[ "$response" == *'"range":{"start":{"line":0,"character":4},"end":{"line":0,"character":7}}'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":10,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$hover_uri"'"},"position":{"line":7,"character":8}}}'
response="$(read_message)"
[[ "$response" == *'"range":{"start":{"line":7,"character":6},"end":{"line":7,"character":13}}'* ]]
count=$((count + 1))

# --- go-to-definition on a local variable returns null too ---

send '{"jsonrpc":"2.0","id":11,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$hover_uri"'"},"position":{"line":1,"character":2}}}'
response="$(read_message)"
[[ "$response" == *'"id":11'* ]]
count=$((count + 1))
[[ "$response" == *'"result":null'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$hover_uri"'"}}}'
read_message >/dev/null

# --- hover on a variadic (splat) function prints "*rest", not the
# generic "rest = ..." optional-parameter form required_arity alone
# would suggest (required_arity never counts the variadic slot as
# required -- see hover.c's own comment on format_function_signature) ---

variadic_hover_uri="file:///variadic_hover.di"
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$variadic_hover_uri"'","text":"def sum(first: Int, *rest)\n  first\nend"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":100,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$variadic_hover_uri"'"},"position":{"line":0,"character":4}}}'
response="$(read_message)"
[[ "$response" == *'"id":100'* ]]
count=$((count + 1))
[[ "$response" == *'"value":"def sum(first: Int, *rest)"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$variadic_hover_uri"'"}}}'
read_message >/dev/null

# --- hover preserves combined variadic and typed block parameter markers ---

block_hover_uri="file:///block_hover.di"
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$block_hover_uri"'","text":"def dispatch(first: Int, *rest, &block: Callable[2])\n  first\nend"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":101,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$block_hover_uri"'"},"position":{"line":0,"character":4}}}'
response="$(read_message)"
[[ "$response" == *'"id":101'* ]]
count=$((count + 1))
[[ "$response" == *'"value":"def dispatch(first: Int, *rest, &block: Callable[2])"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$block_hover_uri"'"}}}'
read_message >/dev/null

# --- hover recognizes the lexical block-presence intrinsic ---

block_given_uri="file:///block_given_hover.di"
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$block_given_uri"'","text":"def present(&block)\n  block_given?()\nend"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":102,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$block_given_uri"'"},"position":{"line":1,"character":4}}}'
response="$(read_message)"
[[ "$response" == *'"id":102'* ]]
count=$((count + 1))
[[ "$response" == *'"value":"block_given?() -> Bool"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$block_given_uri"'"}}}'
read_message >/dev/null

# --- go-to-definition on a symbol pulled in through require resolves to
# a Location in *that* file, not the requesting document ---

definition_main_uri="file://$work/definition_main.di"
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$definition_main_uri"'","text":"require \"helper\"\nputs(greet(\"world\"))"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":12,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$definition_main_uri"'"},"position":{"line":1,"character":6}}}'
response="$(read_message)"
[[ "$response" == *"\"uri\":\"file://$work/helper.di\""* ]]
count=$((count + 1))
[[ "$response" == *'"range":{"start":{"line":0,"character":4},"end":{"line":0,"character":9}}'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$definition_main_uri"'"}}}'
read_message >/dev/null

# --- documentSymbol lists only this document's own top-level declarations:
# not lib/core.di's prelude, not anything pulled in via
# require, and -- the actual bug this caught during development --
# correctly positioned even for a declaration *after* a require line,
# whose raw in-buffer line number is thrown off by the required file's
# own inlined content sitting earlier in the compiled buffer ---

symbol_uri="file://$work/symbols.di"
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$symbol_uri"'","text":"require \"helper\"\ndef mine()\n  1\nend\n\nclass Thing\nend\n\nmine()"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":13,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"'"$symbol_uri"'"}}}'
response="$(read_message)"
[[ "$response" == *'"id":13'* ]]
count=$((count + 1))
[[ "$response" == *'"name":"mine","kind":12,"range":{"start":{"line":1,"character":4},"end":{"line":1,"character":8}}'* ]]
count=$((count + 1))
[[ "$response" == *'"name":"Thing","kind":5,"range":{"start":{"line":5,"character":6},"end":{"line":5,"character":11}}'* ]]
count=$((count + 1))
[[ "$response" != *'"name":"greet"'* ]]
count=$((count + 1))
[[ "$response" != *'"name":"abs"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$symbol_uri"'"}}}'
read_message >/dev/null

# --- documentSymbol on a document with no declarations of its own
# is an empty array, distinct from null (which means "doesn't compile") ---

empty_symbol_uri="file:///empty_symbols.di"
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$empty_symbol_uri"'","text":"puts(1 + 2)"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":14,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"'"$empty_symbol_uri"'"}}}'
response="$(read_message)"
[[ "$response" == *'"result":[]'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$empty_symbol_uri"'"}}}'
read_message >/dev/null

# --- modules and interfaces are first-class declarations on every
# declaration-oriented LSP surface ---

type_symbol_uri="file:///type_symbols.di"
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$type_symbol_uri"'","text":"module Tools\nend\n\ninterface Runnable\nend\n"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":22,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$type_symbol_uri"'"},"position":{"line":0,"character":8}}}'
response="$(read_message)"
[[ "$response" == *'"value":"module Tools"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":23,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$type_symbol_uri"'"},"position":{"line":3,"character":11}}}'
response="$(read_message)"
[[ "$response" == *'"value":"interface Runnable"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":24,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$type_symbol_uri"'"},"position":{"line":0,"character":8}}}'
response="$(read_message)"
[[ "$response" == *'"range":{"start":{"line":0,"character":7},"end":{"line":0,"character":12}}'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":25,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"'"$type_symbol_uri"'"}}}'
response="$(read_message)"
[[ "$response" == *'"name":"Tools","kind":2'* ]]
count=$((count + 1))
[[ "$response" == *'"name":"Runnable","kind":11'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":26,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$type_symbol_uri"'"},"position":{"line":4,"character":3}}}'
response="$(read_message)"
[[ "$response" == *'"label":"Tools","kind":9'* ]]
count=$((count + 1))
[[ "$response" == *'"label":"Runnable","kind":8'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$type_symbol_uri"'"}}}'
read_message >/dev/null

# --- completion suggests locals actually in scope at the cursor (real
# lexical scoping, not name-matching): a rescue-bound name only shows
# up inside its own clause, a name declared after the cursor doesn't
# show up at all, and an outer function's own locals stay visible
# inside a nested closure declared within it (the same capture
# visibility the compiler's own enclosing_locals mechanism grants) ---

completion_uri="file:///completion.di"
completion_source='def outer(a, b)\n  x = a + b\n  begin\n    raise \"boom\"\n  rescue err: String\n    y = err\n  end\n  x\nend\ntop = 42\n'
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$completion_uri"'","text":"'"$completion_source"'"}}}'
read_message >/dev/null

# inside outer's body, right after the a/b parameters and before x is
# assigned: a and b are in scope, x is not yet (declared on this same
# line), nothing rescue-scoped is
send '{"jsonrpc":"2.0","id":15,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$completion_uri"'"},"position":{"line":1,"character":2}}}'
response="$(read_message)"
[[ "$response" == *'"label":"a","kind":6'* ]]
count=$((count + 1))
[[ "$response" == *'"label":"b","kind":6'* ]]
count=$((count + 1))
[[ "$response" == *'"label":"outer","kind":3'* ]]
count=$((count + 1))
[[ "$response" != *'"label":"err"'* ]]
count=$((count + 1))
[[ "$response" != *'"label":"top"'* ]]
count=$((count + 1))

# inside the rescue clause: err (rescue-bound) is in scope, plus a/b/x
# from the enclosing function (closure capture visibility)
send '{"jsonrpc":"2.0","id":16,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$completion_uri"'"},"position":{"line":5,"character":4}}}'
response="$(read_message)"
[[ "$response" == *'"label":"err","kind":6'* ]]
count=$((count + 1))
[[ "$response" == *'"label":"a","kind":6'* ]]
count=$((count + 1))
[[ "$response" == *'"label":"x","kind":6'* ]]
count=$((count + 1))

# at top level, after outer's own `end`: err/y/a/b/x are all correctly
# out of scope (none of them leak past the function that declared
# them), only outer and top (both top-level) are visible
send '{"jsonrpc":"2.0","id":17,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$completion_uri"'"},"position":{"line":9,"character":0}}}'
response="$(read_message)"
[[ "$response" == *'"label":"top","kind":6'* ]]
count=$((count + 1))
[[ "$response" != *'"label":"err"'* ]]
count=$((count + 1))
[[ "$response" != *'"label":"y"'* ]]
count=$((count + 1))
[[ "$response" != *'"label":"x"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$completion_uri"'"}}}'
read_message >/dev/null

# --- completion on a document that doesn't currently compile returns
# null, matching hover/definition/documentSymbol's own rule ---

broken_completion_uri="file:///completion_broken.di"
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$broken_completion_uri"'","text":"def f(\n"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":18,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$broken_completion_uri"'"},"position":{"line":0,"character":4}}}'
response="$(read_message)"
[[ "$response" == *'"result":null'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$broken_completion_uri"'"}}}'
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

# --- receiver metadata is position-sensitive across local reassignment:
# before the second assignment the local completes/resolves as Dog; after
# it, as Cat. End-of-function type snapshots used to make both positions
# incorrectly resolve as whichever class was assigned last. ---

reassigned_uri="file:///reassigned_receiver.di"
reassigned_source='class Dog\n  def bark()\n    1\n  end\nend\nclass Cat\n  def meow()\n    2\n  end\nend\ndef inspect()\n  pet = Dog.new()\n  pet.bark()\n  pet = Cat.new()\n  pet.meow()\nend\ndef inspect_union(pet: Dog | Cat)\n  pet.bark()\n  pet = Cat.new()\n  pet.meow()\nend\nclass Kennel\n  def initialize()\n    @pet = Dog.new()\n  end\n  def speak()\n    @pet.bark()\n  end\nend\nclass Mixed\n  def initialize(flag)\n    if flag\n      @pet = Dog.new()\n    else\n      @pet = Cat.new()\n    end\n  end\n  def speak()\n    @pet.bark()\n  end\nend'
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$reassigned_uri"'","text":"'"$reassigned_source"'"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":110,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$reassigned_uri"'"},"position":{"line":12,"character":6}}}'
response="$(read_message)"
[[ "$response" == *'"label":"bark","kind":3'* ]]
count=$((count + 1))
[[ "$response" != *'"label":"meow","kind":3'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":111,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$reassigned_uri"'"},"position":{"line":14,"character":6}}}'
response="$(read_message)"
[[ "$response" == *'"label":"meow","kind":3'* ]]
count=$((count + 1))
[[ "$response" != *'"label":"bark","kind":3'* ]]
count=$((count + 1))

# A class instance variable resolves across methods when every assignment
# agrees on one concrete class. Conflicting assignments stay unresolved.
send '{"jsonrpc":"2.0","id":118,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$reassigned_uri"'"},"position":{"line":26,"character":9}}}'
response="$(read_message)"
[[ "$response" == *'"label":"bark","kind":3'* ]]
count=$((count + 1))
[[ "$response" != *'"label":"meow","kind":3'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":119,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$reassigned_uri"'"},"position":{"line":26,"character":10}}}'
response="$(read_message)"
[[ "$response" == *'"value":"def bark()"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":120,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$reassigned_uri"'"},"position":{"line":26,"character":10}}}'
response="$(read_message)"
[[ "$response" == *'"start":{"line":1,"character":6}'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":121,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$reassigned_uri"'"},"position":{"line":38,"character":9}}}'
response="$(read_message)"
[[ "$response" != *'"label":"bark","kind":3'* ]]
count=$((count + 1))
[[ "$response" != *'"label":"meow","kind":3'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":112,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$reassigned_uri"'"},"position":{"line":12,"character":7}}}'
response="$(read_message)"
[[ "$response" == *'"value":"def bark()"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":113,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$reassigned_uri"'"},"position":{"line":14,"character":7}}}'
response="$(read_message)"
[[ "$response" == *'"value":"def meow()"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":116,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$reassigned_uri"'"},"position":{"line":12,"character":7}}}'
response="$(read_message)"
[[ "$response" == *'"start":{"line":1,"character":6}'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":117,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$reassigned_uri"'"},"position":{"line":14,"character":7}}}'
response="$(read_message)"
[[ "$response" == *'"start":{"line":6,"character":6}'* ]]
count=$((count + 1))

# An explicitly annotated union remains a union until reassignment, then
# narrows to the concrete class assigned at that source position.
send '{"jsonrpc":"2.0","id":114,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$reassigned_uri"'"},"position":{"line":17,"character":6}}}'
response="$(read_message)"
[[ "$response" == *'"label":"bark","kind":3'* ]]
count=$((count + 1))
[[ "$response" == *'"label":"meow","kind":3'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":115,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$reassigned_uri"'"},"position":{"line":19,"character":6}}}'
response="$(read_message)"
[[ "$response" == *'"label":"meow","kind":3'* ]]
count=$((count + 1))
[[ "$response" != *'"label":"bark","kind":3'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$reassigned_uri"'"}}}'
read_message >/dev/null

# --- explicitly typed call results can themselves be receivers, recursively.
# Cover top-level functions, singleton factories, constructors, several links,
# and a union return. An inferred/unannotated function remains conservative. ---

chained_uri="file:///chained_receiver.di"
chained_source='class Leaf\n  def ping() -> Int\n    1\n  end\nend\nclass Branch\n  def leaf() -> Leaf\n    Leaf.new()\n  end\nend\nclass AlternateBranch\n  def leaf() -> Leaf\n    Leaf.new()\n  end\nend\nclass Factory\n  def self.build() -> Branch\n    Branch.new()\n  end\nend\ndef make_branch() -> Branch\n  Branch.new()\nend\ndef choose_branch(flag) -> Branch | AlternateBranch\n  if flag\n    Branch.new()\n  else\n    AlternateBranch.new()\n  end\nend\ndef infer_branch()\n  Branch.new()\nend\ndef inspect()\n  make_branch().leaf().ping()\n  Factory.build().leaf().ping()\n  Branch.new().leaf().ping()\n  choose_branch(true).leaf().ping()\n  infer_branch().leaf()\n  maybe_branch(true).leaf()\nend\ndef maybe_branch(flag) -> Branch | Nil\n  flag ? Branch.new() : nil\nend'
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$chained_uri"'","text":"'"$chained_source"'"}}}'
read_message >/dev/null

# Top-level function return, first and second hop.
send '{"jsonrpc":"2.0","id":130,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$chained_uri"'"},"position":{"line":34,"character":16}}}'
response="$(read_message)"
[[ "$response" == *'"label":"leaf","kind":3'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":131,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$chained_uri"'"},"position":{"line":34,"character":23}}}'
response="$(read_message)"
[[ "$response" == *'"label":"ping","kind":3'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":132,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$chained_uri"'"},"position":{"line":34,"character":24}}}'
response="$(read_message)"
[[ "$response" == *'"value":"def ping() -> Int"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":133,"method":"textDocument/definition","params":{"textDocument":{"uri":"'"$chained_uri"'"},"position":{"line":34,"character":24}}}'
response="$(read_message)"
[[ "$response" == *'"start":{"line":1,"character":6}'* ]]
count=$((count + 1))

# Singleton method return, direct constructor, and explicit union return.
send '{"jsonrpc":"2.0","id":134,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$chained_uri"'"},"position":{"line":35,"character":25}}}'
response="$(read_message)"
[[ "$response" == *'"label":"ping","kind":3'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":135,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$chained_uri"'"},"position":{"line":36,"character":22}}}'
response="$(read_message)"
[[ "$response" == *'"label":"ping","kind":3'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":136,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$chained_uri"'"},"position":{"line":37,"character":29}}}'
response="$(read_message)"
[[ "$response" == *'"label":"ping","kind":3'* ]]
count=$((count + 1))

# No source annotation means no call-result type claim, even when the body
# happens to return a constructor at runtime.
send '{"jsonrpc":"2.0","id":137,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$chained_uri"'"},"position":{"line":38,"character":17}}}'
response="$(read_message)"
[[ "$response" != *'"label":"leaf","kind":3'* ]]
count=$((count + 1))

# A non-class arm in a declared union is equally unsafe for method lookup.
send '{"jsonrpc":"2.0","id":138,"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$chained_uri"'"},"position":{"line":39,"character":21}}}'
response="$(read_message)"
[[ "$response" != *'"label":"leaf","kind":3'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$chained_uri"'"}}}'
read_message >/dev/null

# --- control-flow joins preserve conservative class unions for receiver
# tooling: expression results, assignments made independently in each branch,
# and ternaries all expose both possible classes after the join. ---

branch_uri="file:///branch_receiver.di"
branch_source='class Dog\n  def bark()\n    1\n  end\nend\nclass Cat\n  def meow()\n    2\n  end\nend\ndef inspect_if(flag)\n  pet = if flag\n    Dog.new()\n  else\n    Cat.new()\n  end\n  pet.bark()\nend\ndef inspect_assign(flag)\n  pet = Dog.new()\n  if flag\n    pet = Dog.new()\n  else\n    pet = Cat.new()\n  end\n  pet.bark()\nend\ndef inspect_ternary(flag)\n  pet = flag ? Dog.new() : Cat.new()\n  pet.bark()\nend\ndef inspect_unless(flag)\n  pet = unless flag\n    Dog.new()\n  else\n    Cat.new()\n  end\n  pet.bark()\nend\ndef inspect_elsif(flag, other)\n  pet = if flag\n    Dog.new()\n  elsif other\n    Cat.new()\n  else\n    Dog.new()\n  end\n  pet.bark()\nend\ndef inspect_case(kind)\n  pet = case kind\n  when 1\n    Dog.new()\n  else\n    Cat.new()\n  end\n  pet.bark()\nend\ndef inspect_while(flag)\n  pet = Cat.new()\n  while flag\n    pet = Dog.new()\n    break\n  end\n  pet.bark()\nend\ndef inspect_loop(flag)\n  pet = loop\n    if flag\n      break Dog.new()\n    else\n      break Cat.new()\n    end\n  end\n  pet.bark()\nend'
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$branch_uri"'","text":"'"$branch_source"'"}}}'
read_message >/dev/null

for request in '140 16' '141 25' '142 29' '143 37' '144 47' '145 56' '146 64' '147 74'; do
  set -- $request
  send '{"jsonrpc":"2.0","id":'"$1"',"method":"textDocument/completion","params":{"textDocument":{"uri":"'"$branch_uri"'"},"position":{"line":'"$2"',"character":6}}}'
  response="$(read_message)"
  [[ "$response" == *'"label":"bark","kind":3'* ]]
  count=$((count + 1))
  [[ "$response" == *'"label":"meow","kind":3'* ]]
  count=$((count + 1))
done

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$branch_uri"'"}}}'
read_message >/dev/null

# --- workspace/symbol recursively walks the workspace root (given via
# initialize's own workspaceFolders, above), skips dotdirs, and
# case-insensitively substring-matches the query against every
# top-level declaration name it finds, across every *.di file, not
# just open documents ---

mkdir -p "$work/ws_sub" "$work/.ws_hidden"
cat > "$work/ws_alpha.di" <<'EOF'
def alpha_fn()
  1
end
EOF
cat > "$work/ws_sub/ws_beta.di" <<'EOF'
def beta_fn()
  2
end
class BetaClass
end
module BetaTools
end
interface BetaRunnable
end
EOF
cat > "$work/.ws_hidden/ws_hidden.di" <<'EOF'
def hidden_fn()
  3
end
EOF

send '{"jsonrpc":"2.0","id":19,"method":"workspace/symbol","params":{"query":""}}'
response="$(read_message)"
[[ "$response" == *'"name":"alpha_fn"'* ]]
count=$((count + 1))
[[ "$response" == *'"name":"beta_fn"'* ]]
count=$((count + 1))
[[ "$response" == *'"name":"BetaClass","kind":5'* ]]
count=$((count + 1))
[[ "$response" == *'"name":"BetaTools","kind":2'* ]]
count=$((count + 1))
[[ "$response" == *'"name":"BetaRunnable","kind":11'* ]]
count=$((count + 1))
[[ "$response" != *'"name":"hidden_fn"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":20,"method":"workspace/symbol","params":{"query":"ALPHA"}}'
response="$(read_message)"
[[ "$response" == *'"name":"alpha_fn","kind":12'* ]]
count=$((count + 1))
[[ "$response" == *"\"uri\":\"file://$work/ws_alpha.di\""* ]]
count=$((count + 1))
[[ "$response" != *'"name":"beta_fn"'* ]]
count=$((count + 1))

# an open document's own live (possibly unsaved) buffer is what gets
# scanned, not stale on-disk content -- same "live over stale" rule
# every other lsp/ feature that resolves source text already follows
ws_alpha_uri="file://$work/ws_alpha.di"
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$ws_alpha_uri"'","text":"def alpha_fn()\n  1\nend\ndef alpha_fn_live()\n  2\nend"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":21,"method":"workspace/symbol","params":{"query":"alpha_fn_live"}}'
response="$(read_message)"
[[ "$response" == *'"name":"alpha_fn_live"'* ]]
count=$((count + 1))
[[ "$(cat "$work/ws_alpha.di")" != *"alpha_fn_live"* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$ws_alpha_uri"'"}}}'
read_message >/dev/null

# --- a div template (packages/div, any ".div" path) is translated on
# the fly and diagnosed directly, instead of being run through the
# compiler as raw text (which would just report the first "<%" as a
# parse error) -- see lsp/div.c ---

div_uri="file:///view.html.div"

# a well-formed template compiles clean
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$div_uri"'","text":"<%# locals: name %><p>Hello <%= name %></p>"}}}'
response="$(read_message)"
[[ "$response" == *'"uri":"'"$div_uri"'"'* ]]
count=$((count + 1))
[[ "$response" == *'"diagnostics":[]'* ]]
count=$((count + 1))

# a broken expression inside <%= %> is reported against the template's
# own single line, not some line deep in the generated boilerplate
send '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$div_uri"'"},"contentChanges":[{"text":"<%# locals: name %><p>Hello <%= name + %></p>"}]}}'
response="$(read_message)"
[[ "$response" == *'"message":"expected expression"'* ]]
count=$((count + 1))
[[ "$response" == *'"line":0'* ]]
count=$((count + 1))

# an error on a <% %> tag's own *second* physical line maps to the
# right line in the template, not the tag's own opening line
send '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$div_uri"'"},"contentChanges":[{"text":"<p>one</p>\n<%\n  if\nend\n%>\n<p>two</p>"}]}}'
response="$(read_message)"
[[ "$response" == *'"message":"expected expression"'* ]]
count=$((count + 1))
[[ "$response" == *'"line":2'* ]]
count=$((count + 1))

# an unterminated <% tag is its own diagnostic, at the tag's own start
send '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$div_uri"'"},"contentChanges":[{"text":"<p>hi</p><%= broken"}]}}'
response="$(read_message)"
[[ "$response" == *'"message":"unterminated <% tag"'* ]]
count=$((count + 1))
[[ "$response" == *'"line":0'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$div_uri"'"}}}'
read_message >/dev/null

# --- local Callable hover retains bound-reference parameter/return graphs ---

callable_local_uri="file:///callable_local_hover.di"
callable_local_source='class HoverOps\n  def increment(value: Int) -> Int = value + 1\n  def wrap[T](value: T) -> Array[T] = [value]\nend\nclass HoverRed\nend\nclass HoverBlue\nend\ndef inspect(value: HoverRed | HoverBlue)\n  ops = HoverOps.new()\n  increment = ops.increment\n  wrapped = ops.wrap[String]\n  increment(1)\n  wrapped\n  items = [1, 2]\n  items\n  union_value = value\n  union_value\nend'
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$callable_local_uri"'","text":"'"$callable_local_source"'"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":170,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$callable_local_uri"'"},"position":{"line":12,"character":4}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Callable[[Int], Int]"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":171,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$callable_local_uri"'"},"position":{"line":13,"character":4}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Callable[[String], Array[String]]"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":172,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$callable_local_uri"'"},"position":{"line":15,"character":3}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Array[Int]"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":173,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$callable_local_uri"'"},"position":{"line":17,"character":7}}}'
response="$(read_message)"
[[ "$response" == *'"value":"HoverRed | HoverBlue"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$callable_local_uri"'"}}}'
read_message >/dev/null

# --- synthesized Callable-union block parameters retain full union graphs ---

union_block_uri="file:///union_block_hover.di"
union_block_source='class LspRed\n  def score() -> Int = 40\nend\nclass LspBlue\n  def score() -> Int = 40\nend\ndef with_red(&block: Callable[[LspRed], Int]) -> Int = yield(LspRed.new())\ndef with_blue(&block: Callable[[LspBlue], Int]) -> Int = yield(LspBlue.new())\nchoice = if ARGV.length() == 0\n  with_red\nelse\n  with_blue\nend\nchoice() do |value|\n  value.score() + 2\nend'
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$union_block_uri"'","text":"'"$union_block_source"'"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":174,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$union_block_uri"'"},"position":{"line":13,"character":14}}}'
response="$(read_message)"
[[ "$response" == *'"value":"LspRed | LspBlue"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$union_block_uri"'"}}}'
read_message >/dev/null

# --- divergent union-receiver returns hover as their safe joined graph ---

union_return_uri="file:///union_return_hover.di"
union_return_source='class ReturnLeft\n  def value() -> Int = 42\nend\nclass ReturnRight\n  def value() -> String = \"forty-two\"\nend\ndef inspect(receiver: ReturnLeft | ReturnRight)\n  joined = receiver.value()\n  joined\nend'
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$union_return_uri"'","text":"'"$union_return_source"'"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":175,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$union_return_uri"'"},"position":{"line":8,"character":4}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Int | String"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$union_return_uri"'"}}}'
read_message >/dev/null

# --- native collection relays retain joined union element graphs ---

collection_relay_uri="file:///collection_relay_hover.di"
collection_relay_source='def inspect(arrays: Array[Int] | Array[String], hashes: Hash[String, Int] | Hash[Symbol, String])
  first = arrays.first()
  first
  reversed = arrays.reverse()
  reversed
  keys = hashes.keys()
  keys
  values = hashes.values()
  values
end'
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$collection_relay_uri"'","text":"'"$collection_relay_source"'"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":179,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$collection_relay_uri"'"},"position":{"line":2,"character":4}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Int | String"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":180,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$collection_relay_uri"'"},"position":{"line":4,"character":5}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Array[Int | String]"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":181,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$collection_relay_uri"'"},"position":{"line":6,"character":3}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Array[String | Symbol]"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":182,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$collection_relay_uri"'"},"position":{"line":8,"character":5}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Array[Int | String]"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$collection_relay_uri"'"}}}'
read_message >/dev/null

# --- argument and callback collection relays construct nested result graphs ---

collection_transform_uri="file:///collection_transform_hover.di"
collection_transform_source='def stringify(value: Int | String) -> String = \"mapped\"
def inspect(arrays: Array[Int] | Array[String], hashes: Hash[String, Int] | Hash[Symbol, String])
  fallback = arrays.first_or(\"fallback\")
  fallback
  concatenated = arrays.concat([\"joined\"])
  concatenated
  mapped = arrays.map(stringify)
  mapped
  sliced = arrays.each_slice(1)
  sliced
  merged = hashes.merge(other: {:joined: \"joined\"})
  merged
  transformed = hashes.map_values(stringify)
  transformed
end'
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$collection_transform_uri"'","text":"'"$collection_transform_source"'"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":183,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$collection_transform_uri"'"},"position":{"line":3,"character":4}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Int | String"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":184,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$collection_transform_uri"'"},"position":{"line":5,"character":6}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Array[Int | String]"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":185,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$collection_transform_uri"'"},"position":{"line":7,"character":4}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Array[String]"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":186,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$collection_transform_uri"'"},"position":{"line":9,"character":4}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Array[Array[Int | String]]"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":187,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$collection_transform_uri"'"},"position":{"line":11,"character":4}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Hash[String | Symbol, Int | String]"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":188,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$collection_transform_uri"'"},"position":{"line":13,"character":5}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Hash[String | Symbol, String]"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$collection_transform_uri"'"}}}'
read_message >/dev/null

# --- scalar, nullable, padded, and Callable-union collection results ---

collection_scalar_uri="file:///collection_scalar_hover.di"
collection_scalar_source='def inspect(arrays: Array[Int] | Array[String], mapper: Callable[[Int | String], String] | Callable[[Int | String], Symbol])
  minimum = arrays.min()
  minimum
  found = arrays.find() do |value|
    value == value
  end
  found
  zipped = arrays.zip([\"joined\"])
  zipped
  mapped = arrays.map(mapper)
  mapped
end'
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$collection_scalar_uri"'","text":"'"$collection_scalar_source"'"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":190,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$collection_scalar_uri"'"},"position":{"line":6,"character":3}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Int | String | Nil"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":191,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$collection_scalar_uri"'"},"position":{"line":8,"character":4}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Array[Array[Int | String | Nil]]"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":192,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$collection_scalar_uri"'"},"position":{"line":10,"character":4}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Array[String | Symbol]"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$collection_scalar_uri"'"}}}'
read_message >/dev/null

# --- mutable collection locals widen from push and indexed writes ---

collection_mutation_uri="file:///collection_mutation_hover.di"
collection_mutation_source='def inspect()
  items = []
  items.push(42)
  items
  items.push(value: \"forty-two\")
  items
  entries = {}
  entries[\"answer\"] = 42
  entries
  entries[:label] = \"forty-two\"
  entries
end
def dynamic(value) = value
def inspect_unknown()
  opaque = [42]
  opaque.push(dynamic(\"dynamic\"))
  opaque
  entries = {\"answer\": 42}
  entries[dynamic(:opaque)] = dynamic(true)
  entries
end'
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$collection_mutation_uri"'","text":"'"$collection_mutation_source"'"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":193,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$collection_mutation_uri"'"},"position":{"line":3,"character":3}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Array[Int]"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":194,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$collection_mutation_uri"'"},"position":{"line":5,"character":3}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Array[Int | String]"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":195,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$collection_mutation_uri"'"},"position":{"line":8,"character":3}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Hash[String, Int]"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":196,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$collection_mutation_uri"'"},"position":{"line":10,"character":3}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Hash[String | Symbol, Int | String]"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":197,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$collection_mutation_uri"'"},"position":{"line":16,"character":3}}}'
response="$(read_message)"
[[ "$response" == *'"result":null'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":198,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$collection_mutation_uri"'"},"position":{"line":19,"character":3}}}'
response="$(read_message)"
[[ "$response" == *'"result":null'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$collection_mutation_uri"'"}}}'
read_message >/dev/null

# --- heterogeneous spread literals bind generic result positions ---

spread_generic_uri="file:///spread_generic_hover.di"
spread_generic_source='def second[T, U](first: T, second: U) -> U = second\ndef suffix[A, B, C](first: A, middle: B, last: C) -> C = last\ndef keyword_middle[A, B, C](first: A, middle: B, last: C) -> B = middle\ndef inspect()\n  joined = second(*[\"ignored\", 42])\n  joined\n  fixed_joined = suffix(true, *[\"middle\"], 42)\n  fixed_joined\n  keyword_joined = keyword_middle(*[true, 42], last: \"done\")\n  keyword_joined\nend'
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$spread_generic_uri"'","text":"'"$spread_generic_source"'"}}}'
read_message >/dev/null

send '{"jsonrpc":"2.0","id":176,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$spread_generic_uri"'"},"position":{"line":5,"character":4}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Int"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":177,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$spread_generic_uri"'"},"position":{"line":7,"character":7}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Int"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","id":178,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$spread_generic_uri"'"},"position":{"line":9,"character":9}}}'
response="$(read_message)"
[[ "$response" == *'"value":"Int"'* ]]
count=$((count + 1))

send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$spread_generic_uri"'"}}}'
read_message >/dev/null

# --- an unrecognized method gets a JSON-RPC MethodNotFound error ---

send '{"jsonrpc":"2.0","id":2,"method":"textDocument/bogusMethod","params":{}}'
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
