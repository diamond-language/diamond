#!/usr/bin/env bash
# A worker without a trap must leave the owner's pending signal intact.
set -euo pipefail
diamond="${1:-./build/diamond}"
output="$(mktemp)"
signalled="$(mktemp -u)"
pid=""
cleanup() {
  if [[ -n "$pid" ]]; then kill -KILL "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
  rm -f "$output" "$signalled"
}
trap cleanup EXIT
program=$(cat <<'DIAMOND'
class Signals
  def self.caught()
    @@caught = true
  end
  def self.check()
    unless @@caught == true then raise "signal lost to another VM" end
  end
end
def busy()
  puts("worker ready")
  deadline = Time.monotonic() + 10.0
  while Time.monotonic() < deadline && !File.exist?(ENV["SIGNALLED"])
    nil
  end
end
Signal.trap("TERM", Signals.caught)
worker = Thread.new(busy)
puts("owner joining")
worker.join()
Signals.check()
puts("caught")
DIAMOND
)
for attempt in 1 2 3 4 5; do
  rm -f "$signalled"
  SIGNALLED="$signalled" "$diamond" -e "$program" > "$output" 2>&1 &
  pid=$!
  ready=false
  for ((probe = 0; probe < 500; probe++)); do
    if grep -q 'worker ready' "$output" && grep -q 'owner joining' "$output"; then ready=true; break; fi
    if ! kill -0 "$pid" 2>/dev/null; then break; fi
    sleep .01
  done
  if [[ "$ready" != true ]]; then cat "$output" >&2; exit 1; fi
  sleep .03
  kill -TERM "$pid"
  touch "$signalled"
  if ! wait "$pid"; then cat "$output" >&2; exit 1; fi
  pid=""
  grep -qx 'caught' "$output"
done
echo 'thread signal ownership tests passed'
