#!/usr/bin/env bash
set -euo pipefail

# diamond-dap is driven over its real stdio transport (Content-Length-framed
# JSON, the identical framing diamond-lsp already uses -- see tests/
# lsp_test.sh's own comment) via a bash coproc, matching every other test
# script in this repo (no Python/Node dependency). Assertions on message
# content are plain substring matches, the same tolerance lsp_test.sh
# already relies on.
#
# DIAMOND_BIN must already point at a real `diamond` binary (the Makefile's
# own `test-dap` target sets it) -- diamond-dap execvp's it to actually run
# the debuggee.

dap="$(realpath ./build/diamond-dap)"
count=0

work="$(realpath "$(mktemp -d)")"
trap 'rm -rf "$work"' EXIT
fixture="$work/fixture.di"
cat > "$fixture" <<'EOF'
def add(a, b)
  x = a + b
  y = x * 2
  y
end

result = add(2, 3)
puts(result)
EOF

send() {
    local body="$1"
    printf 'Content-Length: %d\r\n\r\n%s' "${#body}" "$body" >&"${DAP[1]}"
}

read_message() {
    local line length=-1 body
    while IFS= read -r -u "${DAP[0]}" line; do
        line="${line%$'\r'}"
        [[ -z "$line" ]] && break
        if [[ "$line" == Content-Length:* ]]; then
            length="${line#Content-Length: }"
        fi
    done
    if (( length < 0 )); then
        echo "dap_test: message with no Content-Length header" >&2
        exit 1
    fi
    IFS= read -r -u "${DAP[0]}" -N "$length" body
    printf '%s' "$body"
}

# Reads messages until one containing `pattern` (a plain substring) shows
# up, discarding everything else -- `output` events (the debuggee's own
# stdout/stderr) can interleave with the response/event this test is
# actually waiting for, in either order, so a fixed read count would be
# fragile against exactly how many of those arrive before it.
read_until() {
    local pattern="$1" tries=0 msg
    while (( tries < 30 )); do
        msg="$(read_message)"
        if [[ "$msg" == *"$pattern"* ]]; then
            printf '%s' "$msg"
            return 0
        fi
        tries=$((tries + 1))
    done
    echo "dap_test: never saw a message matching: $pattern" >&2
    exit 1
}

coproc DAP { "$dap"; }

# --- initialize advertises configurationDone support ---

send '{"seq":1,"type":"request","command":"initialize","arguments":{"adapterID":"diamond"}}'
response="$(read_until '"command":"initialize"')"
[[ "$response" == *'"success":true'* ]]
count=$((count + 1))
[[ "$response" == *'"supportsConfigurationDoneRequest":true'* ]]
count=$((count + 1))
read_until '"event":"initialized"' >/dev/null
count=$((count + 1))

# --- setBreakpoints on a line with no explicit debugger() call ---

send '{"seq":2,"type":"request","command":"setBreakpoints","arguments":{"source":{"path":"'"$fixture"'"},"breakpoints":[{"line":3}]}}'
response="$(read_until '"command":"setBreakpoints"')"
[[ "$response" == *'"verified":true'* ]]
count=$((count + 1))

# --- launch (only records the config -- see dap/main.c's own top comment
# for why the real spawn waits for configurationDone) then
# configurationDone actually starts the debuggee ---

send '{"seq":3,"type":"request","command":"launch","arguments":{"program":"'"$fixture"'"}}'
response="$(read_until '"command":"launch"')"
[[ "$response" == *'"success":true'* ]]
count=$((count + 1))

send '{"seq":4,"type":"request","command":"configurationDone"}'
read_until '"command":"configurationDone"' >/dev/null
count=$((count + 1))

# --- the editor breakpoint actually pauses the debuggee ---

stopped="$(read_until '"event":"stopped"')"
[[ "$stopped" == *'"reason":"breakpoint"'* ]]
count=$((count + 1))

send '{"seq":5,"type":"request","command":"stackTrace","arguments":{"threadId":1}}'
response="$(read_until '"command":"stackTrace"')"
[[ "$response" == *'"name":"add"'* ]]
count=$((count + 1))
[[ "$response" == *'"line":3'* ]]
count=$((count + 1))
[[ "$response" == *"\"path\":\"$fixture\""* ]]
count=$((count + 1))

send '{"seq":6,"type":"request","command":"scopes","arguments":{"frameId":0}}'
response="$(read_until '"command":"scopes"')"
[[ "$response" == *'"name":"Locals"'* ]]
count=$((count + 1))

send '{"seq":7,"type":"request","command":"variables","arguments":{"variablesReference":1}}'
response="$(read_until '"command":"variables"')"
[[ "$response" == *'"name":"a","value":"2"'* ]]
count=$((count + 1))
[[ "$response" == *'"name":"x","value":"5"'* ]]
count=$((count + 1))

# --- continue resumes to a clean exit ---

send '{"seq":8,"type":"request","command":"continue","arguments":{"threadId":1}}'
read_until '"command":"continue"' >/dev/null
count=$((count + 1))

read_until '"event":"exited"' >/dev/null
count=$((count + 1))
read_until '"event":"terminated"' >/dev/null
count=$((count + 1))

# --- disconnect: clean exit code 0 ---

send '{"seq":9,"type":"request","command":"disconnect"}'
read_until '"command":"disconnect"' >/dev/null
count=$((count + 1))
wait "$DAP_PID"
count=$((count + 1))

echo "$count dap tests passed"
