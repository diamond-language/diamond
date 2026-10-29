#!/usr/bin/env bash
set -euo pipefail

# Real imports and stdio requests, including an unsaved transitive dependency.
work="$(realpath "$(mktemp -d)")"
trap 'kill "${lsp_pid:-}" 2>/dev/null || true; rm -rf "$work"' EXIT
cp tests/multifile/lsp_generics/*.d6 "$work/"
root_uri="file://$work/main.d6"
model_uri="file://$work/models.d6"
root_source="$(<"$work/main.d6")"
model_source="$(<"$work/models.d6")"

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
    if (( length < 0 )); then echo 'imported generics: missing message header' >&2; exit 1; fi
    IFS= read -r -u "${LSP[0]}" -N "$length" body
    printf '%s' "$body"
}
read_response() {
    local id="$1" body
    while true; do
        body="$(read_message)"
        if [[ "$body" == *'"method":"textDocument/publishDiagnostics"'* ]]; then
            if [[ "$body" != *'"diagnostics":[]'* ]]; then
                echo "imported generics: unexpected diagnostics: $body" >&2; exit 1
            fi
        elif [[ "$body" == *"\"id\":$id,"* ]]; then
            printf '%s' "$body"; return
        else
            echo "imported generics: unexpected response: $body" >&2; exit 1
        fi
    done
}
json_text() {
    local value="$1"
    value="${value//\\/\\\\}"
    value="${value//\"/\\\"}"
    value="${value//$'\n'/\\n}"
    value="${value//$'\r'/\\r}"
    value="${value//$'\t'/\\t}"
    printf '%s' "$value"
}
open_document() {
    send '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$1"'","languageId":"diamond","version":1,"text":"'"$(json_text "$2")"'"}}}'
}
change_document() {
    send '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"'"$1"'","version":'"$2"'},"contentChanges":[{"text":"'"$(json_text "$3")"'"}]}}'
}
request() {
    local id="$1" method="$2" needle="$3" offset="${4:-0}" line row=0 prefix
    while IFS= read -r line; do
        if [[ "$line" == *"$needle"* ]]; then
            prefix="${line%%"$needle"*}$needle"
            send '{"jsonrpc":"2.0","id":'"$id"',"method":"textDocument/'"$method"'","params":{"textDocument":{"uri":"'"$root_uri"'"},"position":{"line":'"$row"',"character":'"$((${#prefix}+offset))"'}}}'
            read_response "$id"; return
        fi
        row=$((row+1))
    done <<< "$root_source"
    echo "imported generics: expression not found: $needle" >&2; exit 1
}
assert_definition_position() {
    local response="$1" source="$2" name="$3" row=0 line prefix
    while IFS= read -r line; do
        if [[ "$line" == *"def $name("* ]]; then
            prefix="${line%%"$name"*}"
            [[ "$response" == *'"start":{"line":'"$row"',"character":'"${#prefix}"'}'* ]]
            return
        fi
        row=$((row+1))
    done <<< "$source"
    echo "imported generics: declaration not found: $name" >&2; exit 1
}

coproc LSP { ./build/diamond-lsp; }
lsp_pid="$LSP_PID"
send '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
read_response 1 >/dev/null
open_document "$model_uri" "$model_source"
open_document "$root_uri" "$root_source"

explicit='ImportedRecords::Box[ImportedModels::Pet].new(ImportedModels::Pet.new()).value().'
response="$(request 2 completion "$explicit")"
[[ "$response" == *'"label":"title"'* && "$response" != *'"label":"misleading"'* ]]
response="$(request 3 hover "$explicit" 1)"
[[ "$response" == *'title'* && "$response" == *'String'* ]]
response="$(request 4 definition "$explicit" 1)"
[[ "$response" == *'"uri":"'"$model_uri"'"'* ]]
assert_definition_position "$response" "$model_source" title

indexed='ImportedRecords::Rows[ImportedModels::Pet].new([ImportedModels::Pet.new()]).value()[0].'
response="$(request 5 completion "$indexed")"
[[ "$response" == *'"label":"title"'* ]]
method='ImportedRecords::Box.new(ImportedModels::Pet.new()).echo[ImportedModels::Leaf](ImportedModels::Leaf.new()).'
response="$(request 6 completion "$method")"
[[ "$response" == *'"label":"ping"'* && "$response" != *'"label":"title"'* ]]
inherited='ImportedRecords::Tagged[ImportedModels::Pet, ImportedModels::Pet].new(ImportedModels::Pet.new(), ImportedModels::Leaf.new(), ImportedModels::Pet.new()).error().'
response="$(request 7 definition "$inherited" 1)"
[[ "$response" == *'"uri":"'"$model_uri"'"'* ]]

# The root remains unchanged while the transitive model import is unsaved.
edited_model="${model_source/def title()/def unsaved_marker() -> String = \"unsaved\"$'\n    'def title()}"
change_document "$model_uri" 2 "$edited_model"
response="$(request 8 completion "$explicit")"
[[ "$response" == *'"label":"unsaved_marker"'* ]]
root_source="${root_source/title()/unsaved_marker()}"
change_document "$root_uri" 2 "$root_source"
response="$(request 9 definition "$explicit" 1)"
[[ "$response" == *'"uri":"'"$model_uri"'"'* ]]
assert_definition_position "$response" "$edited_model" unsaved_marker
response="$(request 10 hover "$explicit" 1)"
[[ "$response" == *'unsaved_marker'* && "$response" == *'String'* ]]

unknown='ImportedRecords::Box.new(value).value().'
response="$(request 11 completion "$unknown")"
[[ "$response" != *'"label":"title"'* && "$response" != *'"label":"misleading"'* ]]
response="$(request 12 definition "$unknown" 1)"
[[ "$response" == *'"result":null'* ]]

root_source="$(<"$work/main.d6")"
change_document "$root_uri" 3 "$root_source"
send '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"'"$model_uri"'"}}}'
response="$(request 13 completion "$explicit")"
[[ "$response" != *'"label":"unsaved_marker"'* ]]
send '{"jsonrpc":"2.0","id":14,"method":"shutdown","params":{}}'
read_response 14 >/dev/null
send '{"jsonrpc":"2.0","method":"exit","params":{}}'
wait "$lsp_pid"
echo 'Imported qualified generic completion, hover, definitions, and unsaved dependencies passed.'
