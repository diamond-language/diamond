#!/usr/bin/env bash
# A trapped signal during sleep() and IO.poll runs its handler, and the wait
# still ends at its original deadline rather than starting over after the
# handler (IO.poll used to retry with its whole timeout). d6 runs directly,
# not under timeout(1), which forwards only the first TERM; the waits below
# bound the test instead.
set -euo pipefail
diamond="${DIAMOND_BIN:-./build/diamond}"
export DIAMOND_NO_CACHE=1
out="$(mktemp)"
pid=""
cleanup() {
    if [[ -n "$pid" ]]; then kill -KILL "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
    rm -f "$out"
}
trap cleanup EXIT

( exec "$diamond" -e 'def handler()
  puts("caught TERM")
end
Signal.trap("TERM", handler)
puts("ready")
start = Time.monotonic()
sleep(1.5)
slept = Time.monotonic() - start
puts("sleep on time: #{slept >= 1.5 && slept < 1.9}")
start = Time.monotonic()
IO.poll([], [], 1500)
waited = Time.monotonic() - start
puts("poll on time: #{waited >= 1.4 && waited < 1.9}")
exit(0)' >"$out" 2>&1 ) &
pid=$!

wait_for_line() {
    for ((attempt = 0; attempt < 200; attempt++)); do
        if grep -qx "$1" "$out"; then return 0; fi
        if ! kill -0 "$pid" 2>/dev/null; then break; fi
        sleep 0.05
    done
    echo "sleep signal test: did not observe '$1'" >&2
    cat "$out" >&2
    return 1
}

wait_for_line ready
sleep 0.7
kill -TERM "$pid"
wait_for_line 'sleep on time: true'
sleep 0.7
kill -TERM "$pid"
wait_for_line 'poll on time: true'
wait "$pid"
pid=""
expected=$'ready\ncaught TERM\nsleep on time: true\ncaught TERM\npoll on time: true'
if [[ "$(cat "$out")" != "$expected" ]]; then
    echo "sleep signal test: unexpected output" >&2
    cat "$out" >&2
    exit 1
fi
echo "sleep signal: ok"
