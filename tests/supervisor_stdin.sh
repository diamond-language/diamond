#!/usr/bin/env bash
# Stop while stdin is open and silent, then hand the pending line to another
# reader. Keep the writer open throughout: EOF must not be what wakes gets().
set -euo pipefail
diamond="${DIAMOND_BIN:-./build/diamond}"
export DIAMOND_NO_CACHE=1
stdin_tmp="$(mktemp -d)"
stdin_pid=""
cleanup() {
    if [[ -n "$stdin_pid" ]]; then
        kill -TERM "$stdin_pid" 2>/dev/null || true
        wait "$stdin_pid" 2>/dev/null || true
    fi
    exec 3>&- || true
    rm -rf "$stdin_tmp"
}
trap cleanup EXIT
mkfifo "$stdin_tmp/input"
exec 3<>"$stdin_tmp/input"
cat > "$stdin_tmp/reader.d6" <<'DIAMOND'
def reader(ready)
  ready.send(true)
  gets()
end

ready = Channel.new(1)
sup = Supervisor.new()
sup.add_child(reader, ready)
ready.receive()
# The second worker queues behind the one that owns the stdin read lock.
sup.add_child(reader, ready)
ready.receive()
puts("reading")
started = Time.monotonic()
puts(sup.stop(500))
puts(Time.monotonic() - started < 2.0)
puts(sup.alive?(0))
puts(sup.alive?(1))
puts("stopped")
# More interrupted readers must reuse the pending read, not create abandoned
# consumers that steal the line supplied after all of them have stopped.
i = 0
while i < 12
  next_sup = Supervisor.new()
  next_sup.add_child(reader, ready)
  ready.receive()
  next_sup.stop(0)
  i += 1
end
puts("reused")
puts(gets())
puts(gets())
puts("done")
nil
DIAMOND
timeout 15 "$diamond" "$stdin_tmp/reader.d6" <"$stdin_tmp/input" >"$stdin_tmp/output" 2>&1 &
stdin_pid=$!
wait_for_line() {
    local line="$1"
    for ((attempt = 0; attempt < 200; attempt++)); do
        if grep -qx "$line" "$stdin_tmp/output"; then return 0; fi
        if ! kill -0 "$stdin_pid" 2>/dev/null; then break; fi
        sleep 0.025
    done
    cat "$stdin_tmp/output" >&2
    echo "stdin test: did not observe '$line'" >&2
    return 1
}
wait_for_line reading
printf 'partial ' >&3
wait_for_line stopped
wait_for_line reused
printf 'line\r\nnext\n' >&3
wait_for_line done
wait "$stdin_pid"
stdin_pid=""
expected=$'reading\nfalse\ntrue\nfalse\nfalse\nstopped\nreused\npartial line\nnext\ndone\nnil'
if [[ "$(cat "$stdin_tmp/output")" != "$expected" ]]; then
    cat "$stdin_tmp/output" >&2
    echo "stdin test: unexpected output" >&2
    exit 1
fi

# Exiting after an interrupted read must not wait for stdin to become ready.
timeout 5 "$diamond" -e 'def reader(); gets(); end; s = Supervisor.new(); s.add_child(reader); s.stop(100); nil' \
    <"$stdin_tmp/input" >"$stdin_tmp/exit-output" 2>&1
[[ "$(cat "$stdin_tmp/exit-output")" == nil ]]

# A sibling crash uses the same interruption path. The restarted reader
# finishes without input; join must not wait for the first attempt's gets().
timeout 5 "$diamond" -e '
def reader(starts)
  starts.send(true)
  gets() if starts.size() == 1
end
def crasher(starts, crashes)
  crashes.send(true)
  if crashes.size() == 1
    loop
      break if starts.size() > 0
    end
    IO.poll([], [], 50)
    raise "restart sibling"
  end
end
starts = Channel.new(4)
crashes = Channel.new(4)
s = Supervisor.new(:one_for_all)
s.add_child(reader, starts)
s.add_child(crasher, starts, crashes)
s.join()
puts([s.restart_count(0), s.restart_count(1)].inspect())
nil' <"$stdin_tmp/input" >"$stdin_tmp/restart-output" 2>&1
[[ "$(cat "$stdin_tmp/restart-output")" == $'[1, 1]\nnil' ]]

# Complete lines are handed to concurrent readers intact, including CRLF,
# empty lines, and the unterminated final line. EOF remains nil.
printf 'first\r\n\nlast' | timeout 5 "$diamond" -e '
def reader(); gets(); end
a = Thread.new(reader)
b = Thread.new(reader)
c = Thread.new(reader)
puts([a.join(), b.join(), c.join()].sort().inspect())
puts(gets())
nil' >"$stdin_tmp/concurrent-output" 2>&1
[[ "$(cat "$stdin_tmp/concurrent-output")" == $'["", "first", "last"]\nnil\nnil' ]]
