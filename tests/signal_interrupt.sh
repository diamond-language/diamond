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
        pkill -KILL -P "$signal_pid" 2>/dev/null || true
        kill -KILL "$signal_pid" 2>/dev/null || true
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
# timeout(1) forwards the INT to the child and, outside --foreground, to its own
# process group as well, so the handler can legitimately run twice depending on
# whether the second delivery lands before or after the first handler ran.
# Collapse adjacent repeats: the contract is that blocked accept() is interrupted
# and resumes, not how many copies of the signal arrived.
actual="$(uniq "$signal_out")"
if [[ "$actual" != $'ready\ncaught INT\naccepted\nnil' ]]; then
    echo "signal test: unexpected output" >&2
    cat "$signal_out" >&2
    exit 1
fi

# A trapped signal that arrives before accept() starts blocking -- here while
# JIT-compiled code runs, which has no per-instruction signal check -- must
# still run its handler while accept() waits, not only once a client connects
# (wait_fd_watching_signals). Before that, "caught TERM" never appeared here.
early_port=18750
early_out="$(mktemp)"
( DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=1 exec timeout -s KILL 30 "$diamond" -e "$(printf 'def run()
  def handler()
    puts("caught TERM")
  end
  Signal.trap("TERM", handler)
  server = TCPServer.listen(%d)
  child = Process.spawn(["sh", "-c", "kill -TERM $PPID"])
  child.wait()
  puts("ready")
  conn = server.accept()
  puts("accepted")
  conn.close()
  server.close()
end
run()' "$early_port")" >"$early_out" 2>&1 ) &
signal_pid=$!
rm -f "$signal_out"
signal_out="$early_out"
# Without the JIT the handler may run before "ready"; either order is fine.
wait_for_line 'caught TERM'
exec 3<>"/dev/tcp/127.0.0.1/$early_port"
{ exec 3<&- 3>&-; } 2>/dev/null || true
wait "$signal_pid" || true
signal_pid=""
if [[ "$(grep -vx ready "$early_out")" != $'caught TERM\naccepted\nnil' ]]; then
    echo "signal test: early TERM: unexpected output" >&2
    cat "$early_out" >&2
    exit 1
fi
rm -f "$early_out"
