#!/usr/bin/env bash
set -euo pipefail

# A self-contained version of SkindicateV3's imported schema -> generated
# reader -> query workflow. No sibling checkout, installed cuts, or database
# is required. Exercise the real protocol and the compiler's actual traces.
work="$(realpath "$(mktemp -d)")"
trap 'kill "${lsp_pid:-}" 2>/dev/null || true; rm -rf "$work"' EXIT
root_uri="file://$work/main.di"
query_uri="file://$work/query.di"
query_initial='class Query\n  def initial() -> Query = self\n  def where(expression: String) -> Query = self\n  def order_by(field: String) -> Query = self\n  def limit(count: Int) -> Query = self\nend\n'
# Move limit's declaration and change its signature as well as its member set.
query_edited='class Query\n\n  def refreshed() -> Query = self\n  def where(expression: String) -> Query = self\n  def order_by(field: String) -> Query = self\n  def limit(size: Int) -> Query = self\nend\n'
printf '%b' "$query_initial" > "$work/query.di"
cat > "$work/schema.di" <<'EOF'
require "./query"
class SkinSchema
  def query() = Query.new()
end
class SchemaReaders
  attr_reader skins
end
class Schemas < SchemaReaders
  def initialize()
    @skins = SkinSchema.new()
  end
end
EOF
root_source='require \"./schema\"\ndef skin_query() = skin_schema().query()\ndef skin_schema() = wrap_schemas().skins()\ndef wrap_schemas() = build_schemas()\ndef build_schemas() = Schemas.new()\ndef inspect_query()\n  query = skin_query()\n  query.limit(20)\n  skin_query().limit(20)\nend\n'

send() {
    local body="$1"
    printf 'Content-Length: %d\r\n\r\n%s' "${#body}" "$body" >&"${LSP[1]}"
}
read_message() {
    local line length=-1 body
    while IFS= read -r -u "${LSP[0]}" line; do
        line="${line%$'\r'}"
        [[ -z "$line" ]] && break
        if [[ "$line" == Content-Length:* ]]; then length="${line#Content-Length: }"; fi
    done
    if (( length < 0 )); then echo 'V3 editor: missing message header' >&2; exit 1; fi
    IFS= read -r -u "${LSP[0]}" -N "$length" body
    printf '%s' "$body"
}
allow_errors=false
read_response() {
    local id="$1" body
    while true; do
        body="$(read_message)"
        if [[ "$body" == *'"method":"textDocument/publishDiagnostics"'* ]]; then
            if [[ "$allow_errors" == false && "$body" != *'"diagnostics":[]'* ]]; then
                echo "V3 editor: unexpected diagnostics: $body" >&2; exit 1
            fi
        elif [[ "$body" == *"\"id\":$id,"* ]]; then
            printf '%s' "$body"; return
        else
            echo "V3 editor: unexpected response: $body" >&2; exit 1
        fi
    done
}
request() {
    send '{"jsonrpc":"2.0","id":'"$1"',"method":"textDocument/'"$2"'","params":{"textDocument":{"uri":"'"$root_uri"'"},"position":{"line":'"$3"',"character":'"$4"'}}}'
    read_response "$1"
}
compile_count() { grep -c '^compile: discovery ' "$work/trace"; }
check_query() {
    local member="$1" absent="$2" parameter="$3" declaration_line="$4" response count
    response="$(request 10 completion 7 8)"
    for name in where order_by limit "$member"; do
        [[ "$response" == *"\"label\":\"$name\",\"kind\":3"* ]]
    done
    [[ "$response" != *"\"label\":\"$absent\""* ]]
    count="$(compile_count)"
    response="$(request 11 hover 7 10)"
    [[ "$response" == *"def limit($parameter: Int)"* ]]
    response="$(request 12 definition 7 10)"
    [[ "$response" == *"\"uri\":\"$query_uri\""* ]]
    [[ "$response" == *"\"start\":{\"line\":$declaration_line,\"character\":6}"* ]]
    response="$(request 13 completion 8 15)"
    [[ "$response" == *'"label":"limit","kind":3'* ]]
    [[ "$response" == *"\"label\":\"$member\",\"kind\":3"* ]]
    # Hover, definition, and direct-chain completion reuse the first analysis.
    [[ "$(compile_count)" == "$count" ]]
}

coproc LSP { DIAMOND_TRACE_COMPILE=1 ./build/diamond-lsp 2>"$work/trace"; }
lsp_pid="$LSP_PID"
send '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"workspaceFolders":[{"uri":"file://'"$work"'","name":"V3"}]}}'
read_response 1 >/dev/null
send '{"jsonrpc":"2.0","method":"initialized","params":{}}'
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$root_uri"'","text":"'"$root_source"'"}}}'
check_query initial refreshed count 4

# Warm the cache, then edit a transitive import without saving or changing main.
send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$query_uri"'","text":"'"$query_initial"'"}}}'
check_query initial refreshed count 4
send '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$query_uri"'"},"contentChanges":[{"text":"'"$query_edited"'"}]}}'
check_query refreshed initial size 5
[[ "$(cat "$work/query.di")" != *refreshed* ]]

# A broken imported buffer cannot expose the preceding successful analysis.
allow_errors=true
send '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$query_uri"'"},"contentChanges":[{"text":"class Query\n  def broken(\n"}]}}'
for method in completion hover definition; do
    response="$(request 20 "$method" 7 10)"
    [[ "$response" == *'"result":null'* ]]
done
allow_errors=false
send '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$query_uri"'"},"contentChanges":[{"text":"'"$query_edited"'"}]}}'
check_query refreshed initial size 5

# Closing the unsaved import restores disk facts. A later disk edit must also
# invalidate the same root's cached analysis without a document notification.
send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$query_uri"'"}}}'
check_query initial refreshed count 4
printf '%b' "$query_edited" > "$work/query.di"
check_query refreshed initial size 5

# Method signatures must exclude the implicit receiver when counting required,
# optional, variadic, and explicit block slots. Top-level functions retain all
# their declared slots. Cover singleton methods too.
signature_source='class Signature\n  def required(first: Int, second: Int) -> Int = first\n  def optional(first: Int = 1, second: Int = 2) -> Int = first\n  def variadic(*items) = nil\n  def block(&callback) = nil\n  def mixed(first: Int = 1, *items, &callback) = nil\n  def self.create(first: Int = 1, *items, &callback) = nil\n  def zero() = nil\nend\ndef standalone(first: Int = 1, *items, &callback) = nil\ndef inspect_signatures(sig: Signature)\n  sig.required(1, 2)\n  sig.optional()\n  sig.variadic()\n  sig.block()\n  sig.mixed()\n  sig.zero()\n  Signature.create()\n  standalone()\nend\n'
send '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$root_uri"'"},"contentChanges":[{"text":"'"$signature_source"'"}]}}'
while IFS='|' read -r line character expected; do
    response="$(request 30 hover "$line" "$character")"
    [[ "$response" == *"\"value\":\"$expected\""* ]]
done <<'EOF'
11|8|def required(first: Int, second: Int) -> Int
12|8|def optional(first: Int = ..., second: Int = ...) -> Int
13|8|def variadic(*items)
14|8|def block(&callback)
15|8|def mixed(first: Int = ..., *items, &callback)
16|8|def zero()
17|14|def create(first: Int = ..., *items, &callback)
18|4|def standalone(first: Int = ..., *items, &callback)
EOF

send '{"jsonrpc":"2.0","id":99,"method":"shutdown","params":{}}'
read_response 99 >/dev/null
send '{"jsonrpc":"2.0","method":"exit","params":{}}'
wait "$lsp_pid"
lsp_pid=''
echo 'V3 imported query workflow, cache reuse, and edit recovery passed'
echo 'method hover parameter signatures passed'
