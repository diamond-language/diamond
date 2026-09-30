#!/usr/bin/env bash
# Signal delivery while blocked in native accept(). Do not probe the port:
# that would consume the server's only accept() before the real client.
set -euo pipefail
diamond="${DIAMOND_BIN:-./build/diamond}"
export DIAMOND_NO_CACHE=1
signal_port=18749
signal_out="$(mktemp)"
signal_pid=""
cleanup() {
    if [[ -n "$signal_pid" ]]; then
        kill -TERM "$signal_pid" 2>/dev/null || true
        wait "$signal_pid" 2>/dev/null || true
    fi
    rm -f "$signal_out"
}
trap cleanup EXIT

# Reset bash's background SIGINT disposition before starting timeout, which
# forwards INT to Diamond. Leave enough time for both bounded handshakes.
( trap - INT
  exec timeout 30 "$diamond" -e "$(printf 'def run()
  def handler()
    puts("caught INT")
  end
  Signal.trap("INT", handler)
  server = TCPServer.listen(%d)
  puts("ready")
  conn = server.accept()
  puts("accepted")
  conn.close()
  server.close()
end
run()' "$signal_port")" >"$signal_out" 2>&1 ) &
signal_pid=$!

wait_for_line() {
    local line="$1"
    for ((attempt = 0; attempt < 200; attempt++)); do
        if grep -qx "$line" "$signal_out"; then return 0; fi
        if ! kill -0 "$signal_pid" 2>/dev/null; then break; fi
        sleep 0.05
    done
    echo "signal test: did not observe '$line' before exit or deadline" >&2
    cat "$signal_out" >&2
    return 1
}

wait_for_line ready
kill -INT "$signal_pid"
# Receiving the handler's acknowledgement before the client connects is
# essential: a fixed sleep can hide failure to interrupt blocked accept().
wait_for_line 'caught INT'
exec 3<>"/dev/tcp/127.0.0.1/$signal_port"
{ exec 3<&- 3>&-; } 2>/dev/null || true
# GNU timeout reports the child's status; uutils may report the forwarded
# signal's status instead. Validate the complete output on either platform.
wait "$signal_pid" || true
signal_pid=""
actual="$(cat "$signal_out")"
if [[ "$actual" != $'ready\ncaught INT\naccepted\nnil' ]]; then
    echo "signal test: unexpected output" >&2
    cat "$signal_out" >&2
    exit 1
fi
