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
result2 = add(10, 20)
puts(result2)
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
# Bash unsets a named coprocess's PID variable after noticing that it exited;
# preserve it now so a fast clean shutdown cannot race the later wait under -u.
dap_pid="$DAP_PID"

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

# --- live add: while already stopped (no restart), arm a line that was
# never in the initial breakpoint set at all -- setBreakpoints replaces
# the whole set for this source, so this also implicitly drops line 3 ---

send '{"seq":8,"type":"request","command":"setBreakpoints","arguments":{"source":{"path":"'"$fixture"'"},"breakpoints":[{"line":4}]}}'
response="$(read_until '"command":"setBreakpoints"')"
[[ "$response" == *'"verified":true'* ]]
count=$((count + 1))

send '{"seq":9,"type":"request","command":"continue","arguments":{"threadId":1}}'
read_until '"command":"continue"' >/dev/null
count=$((count + 1))

stopped="$(read_until '"event":"stopped"')"
[[ "$stopped" == *'"reason":"breakpoint"'* ]]
count=$((count + 1))

send '{"seq":10,"type":"request","command":"stackTrace","arguments":{"threadId":1}}'
response="$(read_until '"command":"stackTrace"')"
[[ "$response" == *'"line":4'* ]]
count=$((count + 1))

# --- live remove: clear every breakpoint while still stopped, then
# confirm the second add() call (same shape as the first) does NOT pause
# again -- proves removal, not just that nothing new was ever added ---

send '{"seq":11,"type":"request","command":"setBreakpoints","arguments":{"source":{"path":"'"$fixture"'"},"breakpoints":[]}}'
read_until '"command":"setBreakpoints"' >/dev/null
count=$((count + 1))

send '{"seq":12,"type":"request","command":"continue","arguments":{"threadId":1}}'
read_until '"command":"continue"' >/dev/null
count=$((count + 1))

saw_unexpected_stop=0
tries=0
while (( tries < 30 )); do
    msg="$(read_message)"
    if [[ "$msg" == *'"event":"stopped"'* ]]; then
        saw_unexpected_stop=1
        break
    fi
    if [[ "$msg" == *'"event":"exited"'* ]]; then
        break
    fi
    tries=$((tries + 1))
done
[[ "$saw_unexpected_stop" == "0" ]]
count=$((count + 1))
read_until '"event":"terminated"' >/dev/null
count=$((count + 1))

# --- disconnect: clean exit code 0 ---

send '{"seq":13,"type":"request","command":"disconnect"}'
read_until '"command":"disconnect"' >/dev/null
count=$((count + 1))
wait "$dap_pid"
count=$((count + 1))

# --- a fresh session that starts with ZERO breakpoints selected before
# configurationDone still lets a live setBreakpoints pause it later --
# proves DIAMOND_DEBUG_FD alone (not just a nonempty initial set) is
# enough to fully instrument the debuggee. Uses an explicit debugger()
# call as a deterministic synchronization point (no breakpoint/timing
# race needed): it always pauses regardless of any armed-line set, the
# same way it already does with no debugger attached at all. ---

fixture2="$work/fixture2.di"
cat > "$fixture2" <<'EOF'
def helper()
  debugger()
  first = 1
  second = 2
  second
end

result = helper()
puts(result)
EOF

coproc DAP2 { "$dap"; }
dap2_pid="$DAP2_PID"

send2() {
    local body="$1"
    printf 'Content-Length: %d\r\n\r\n%s' "${#body}" "$body" >&"${DAP2[1]}"
}
read_message2() {
    local line length=-1 body
    while IFS= read -r -u "${DAP2[0]}" line; do
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
    IFS= read -r -u "${DAP2[0]}" -N "$length" body
    printf '%s' "$body"
}
read_until2() {
    local pattern="$1" tries=0 msg
    while (( tries < 30 )); do
        msg="$(read_message2)"
        if [[ "$msg" == *"$pattern"* ]]; then
            printf '%s' "$msg"
            return 0
        fi
        tries=$((tries + 1))
    done
    echo "dap_test: never saw a message matching: $pattern" >&2
    exit 1
}

send2 '{"seq":1,"type":"request","command":"initialize","arguments":{"adapterID":"diamond"}}'
read_until2 '"command":"initialize"' >/dev/null
count=$((count + 1))
read_until2 '"event":"initialized"' >/dev/null
count=$((count + 1))

# No setBreakpoints call at all -- launch straight from initialize.

send2 '{"seq":2,"type":"request","command":"launch","arguments":{"program":"'"$fixture2"'"}}'
read_until2 '"command":"launch"' >/dev/null
count=$((count + 1))

send2 '{"seq":3,"type":"request","command":"configurationDone"}'
read_until2 '"command":"configurationDone"' >/dev/null
count=$((count + 1))

stopped="$(read_until2 '"event":"stopped"')"
[[ "$stopped" == *'"reason":"breakpoint"'* ]]
count=$((count + 1))

send2 '{"seq":4,"type":"request","command":"stackTrace","arguments":{"threadId":1}}'
response="$(read_until2 '"command":"stackTrace"')"
[[ "$response" == *'"line":2'* ]]
count=$((count + 1))

# Live-arm a line that was never selected before this debuggee even
# started -- the only way it can ever pause there.

send2 '{"seq":5,"type":"request","command":"setBreakpoints","arguments":{"source":{"path":"'"$fixture2"'"},"breakpoints":[{"line":4}]}}'
read_until2 '"command":"setBreakpoints"' >/dev/null
count=$((count + 1))

send2 '{"seq":6,"type":"request","command":"continue","arguments":{"threadId":1}}'
read_until2 '"command":"continue"' >/dev/null
count=$((count + 1))

stopped="$(read_until2 '"event":"stopped"')"
[[ "$stopped" == *'"reason":"breakpoint"'* ]]
count=$((count + 1))

send2 '{"seq":7,"type":"request","command":"stackTrace","arguments":{"threadId":1}}'
response="$(read_until2 '"command":"stackTrace"')"
[[ "$response" == *'"line":4'* ]]
count=$((count + 1))

send2 '{"seq":8,"type":"request","command":"continue","arguments":{"threadId":1}}'
read_until2 '"command":"continue"' >/dev/null
count=$((count + 1))
read_until2 '"event":"exited"' >/dev/null
count=$((count + 1))
read_until2 '"event":"terminated"' >/dev/null
count=$((count + 1))

send2 '{"seq":9,"type":"request","command":"disconnect"}'
read_until2 '"command":"disconnect"' >/dev/null
count=$((count + 1))
wait "$dap2_pid"
count=$((count + 1))

# --- real stepping: next/stepIn/stepOut, chained through one continuous
# session. A fixture where every call is a leaf can't distinguish
# step-over from step-in, so this one has outer() call inner(). ---

fixture3="$work/fixture3.di"
cat > "$fixture3" <<'EOF'
def inner()
  a = 1
  b = 2
  b
end

def outer()
  x = inner()
  y = x + 1
  y
end

result = outer()
puts(result)
EOF

coproc DAP3 { "$dap"; }
dap3_pid="$DAP3_PID"

send3() {
    local body="$1"
    printf 'Content-Length: %d\r\n\r\n%s' "${#body}" "$body" >&"${DAP3[1]}"
}
read_message3() {
    local line length=-1 body
    while IFS= read -r -u "${DAP3[0]}" line; do
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
    IFS= read -r -u "${DAP3[0]}" -N "$length" body
    printf '%s' "$body"
}
read_until3() {
    local pattern="$1" tries=0 msg
    while (( tries < 30 )); do
        msg="$(read_message3)"
        if [[ "$msg" == *"$pattern"* ]]; then
            printf '%s' "$msg"
            return 0
        fi
        tries=$((tries + 1))
    done
    echo "dap_test: never saw a message matching: $pattern" >&2
    exit 1
}

send3 '{"seq":1,"type":"request","command":"initialize","arguments":{"adapterID":"diamond"}}'
read_until3 '"command":"initialize"' >/dev/null
count=$((count + 1))
read_until3 '"event":"initialized"' >/dev/null
count=$((count + 1))

send3 '{"seq":2,"type":"request","command":"setBreakpoints","arguments":{"source":{"path":"'"$fixture3"'"},"breakpoints":[{"line":8}]}}'
read_until3 '"command":"setBreakpoints"' >/dev/null
count=$((count + 1))

send3 '{"seq":3,"type":"request","command":"launch","arguments":{"program":"'"$fixture3"'"}}'
read_until3 '"command":"launch"' >/dev/null
count=$((count + 1))

send3 '{"seq":4,"type":"request","command":"configurationDone"}'
read_until3 '"command":"configurationDone"' >/dev/null
count=$((count + 1))

stopped="$(read_until3 '"event":"stopped"')"
[[ "$stopped" == *'"reason":"breakpoint"'* ]]
count=$((count + 1))

send3 '{"seq":5,"type":"request","command":"stackTrace","arguments":{"threadId":1}}'
response="$(read_until3 '"command":"stackTrace"')"
[[ "$response" == *'"name":"outer"'* ]]
count=$((count + 1))
[[ "$response" == *'"line":8'* ]]
count=$((count + 1))

# --- stepIn: from the call site, land inside inner()'s own first
# statement, one frame deeper ---

send3 '{"seq":6,"type":"request","command":"stepIn","arguments":{"threadId":1}}'
read_until3 '"command":"stepIn"' >/dev/null
count=$((count + 1))

stopped="$(read_until3 '"event":"stopped"')"
[[ "$stopped" == *'"reason":"step"'* ]]
count=$((count + 1))

send3 '{"seq":7,"type":"request","command":"stackTrace","arguments":{"threadId":1}}'
response="$(read_until3 '"command":"stackTrace"')"
[[ "$response" == *'"name":"inner"'* ]]
count=$((count + 1))
[[ "$response" == *'"line":2'* ]]
count=$((count + 1))

# --- stepOut: back in outer(), right after the call returns ---

send3 '{"seq":8,"type":"request","command":"stepOut","arguments":{"threadId":1}}'
read_until3 '"command":"stepOut"' >/dev/null
count=$((count + 1))

stopped="$(read_until3 '"event":"stopped"')"
[[ "$stopped" == *'"reason":"step"'* ]]
count=$((count + 1))

send3 '{"seq":9,"type":"request","command":"stackTrace","arguments":{"threadId":1}}'
response="$(read_until3 '"command":"stackTrace"')"
[[ "$response" == *'"name":"outer"'* ]]
count=$((count + 1))
[[ "$response" == *'"line":9'* ]]
count=$((count + 1))

# --- next (step over): stays in outer(), doesn't descend into anything ---

send3 '{"seq":10,"type":"request","command":"next","arguments":{"threadId":1}}'
read_until3 '"command":"next"' >/dev/null
count=$((count + 1))

stopped="$(read_until3 '"event":"stopped"')"
[[ "$stopped" == *'"reason":"step"'* ]]
count=$((count + 1))

send3 '{"seq":11,"type":"request","command":"stackTrace","arguments":{"threadId":1}}'
response="$(read_until3 '"command":"stackTrace"')"
[[ "$response" == *'"name":"outer"'* ]]
count=$((count + 1))
[[ "$response" == *'"line":10'* ]]
count=$((count + 1))

send3 '{"seq":12,"type":"request","command":"continue","arguments":{"threadId":1}}'
read_until3 '"command":"continue"' >/dev/null
count=$((count + 1))
read_until3 '"event":"exited"' >/dev/null
count=$((count + 1))
read_until3 '"event":"terminated"' >/dev/null
count=$((count + 1))

send3 '{"seq":13,"type":"request","command":"disconnect"}'
read_until3 '"command":"disconnect"' >/dev/null
count=$((count + 1))
wait "$dap3_pid"
count=$((count + 1))

echo "$count dap tests passed"
