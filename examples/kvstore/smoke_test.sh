#!/usr/bin/env bash
# Starts kvserver (built with `diamond build`) on a free port and talks to
# it with kvclient and raw /dev/tcp: commands, expiry, split and pipelined
# requests, concurrent clients, log compaction, SIGTERM shutdown, and
# reloading the log after a restart. Set DIAMOND_BIN to use a diamond
# other than ../../build/diamond, and KV_PORT to pick the port.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
port="${KV_PORT:-18000}"
work="$(mktemp -d)"
server_pid=""
cleanup() { [[ -n "$server_pid" ]] && kill "$server_pid" 2> /dev/null || true; rm -rf "$work"; }
trap cleanup EXIT

"$diamond" build kvserver.di -o "$work/kvserver" > "$work/build.log"
"$diamond" build kvclient.di -o "$work/kvclient" > "$work/build.log"
kv() { "$work/kvclient" "$port" "$@"; }
start() {
  # bash starts background jobs with SIGINT ignored; reset it (docs/networking.md).
  (trap - INT TERM; exec "$work/kvserver" "$port" "$work/data.log") > "$work/server.out" 2>&1 &
  server_pid=$!
  for _ in $(seq 50); do kv PING > /dev/null 2>&1 && return 0; sleep 0.1; done
  echo "server didn't start" >&2; cat "$work/server.out" >&2; exit 1
}
stop() {
  kill -TERM "$server_pid"; wait "$server_pid"; server_pid=""
  grep -q "stopped with" "$work/server.out"
}

start
[[ "$(kv 'SET greeting hello world' 'GET greeting' | tr '\n' '|')" == "+OK|\$hello world|" ]]
[[ "$(kv 'INCR hits' 'INCR hits' 'GET nope' 'DEL nope' | tr '\n' '|')" == ":1|:2|\$|:0|" ]]
kv 'SETEX temp 1 short lived' > /dev/null
[[ "$(kv 'TTL temp')" == ":1" ]]
sleep 1.2
[[ "$(kv 'GET temp' 'TTL temp' | tr '\n' '|')" == "\$|:-2|" ]]
status=0; kv 'BOGUS' > /dev/null || status=$?; [[ "$status" == 1 ]]

# One request split across two packets, then two in one packet.
exec 3<> "/dev/tcp/127.0.0.1/$port"
printf 'SE' >&3; sleep 0.2; printf 'T split yes\nGET split\nGET greeting\n' >&3
read -r one <&3; read -r two <&3; read -r three <&3
exec 3>&-
[[ "$one|$two|$three" == "+OK|\$yes|\$hello world" ]]

# Twenty clients at once, each incrementing the same counter five times.
clients=()
for i in $(seq 20); do
  kv 'INCR shared' 'INCR shared' 'INCR shared' 'INCR shared' 'INCR shared' > /dev/null &
  clients+=($!)
done
wait "${clients[@]}"
[[ "$(kv 'GET shared')" == "\$100" ]]

# Many overwrites bloat the log; the once-a-second maintenance compacts it.
for i in $(seq 60); do echo "SET churn $i"; done | kv > /dev/null
sleep 1.5
lines=$(wc -l < "$work/data.log")
(( lines < 20 )) || { echo "log not compacted: $lines lines" >&2; exit 1; }

stop
start
[[ "$(kv 'GET greeting' 'GET shared' 'GET churn' 'KEYS s' | tr '\n' '|')" == "\$hello world|\$100|\$60|*2 shared split|" ]]
grep -q "5 key(s) loaded" "$work/server.out"
stop
echo "kvstore smoke test passed"
