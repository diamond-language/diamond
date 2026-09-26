# examples/kvstore

A small networked key-value store: a single-threaded TCP server built on
non-blocking sockets and one `IO.poll` loop, with expiring keys, an
append-only log that survives restarts, and a clean shutdown on SIGTERM.

```text
$ diamond kvserver.di 7000 data.log &
kvserver: 0 key(s) loaded, listening on port 7000
$ diamond kvclient.di 7000 'SET greeting hello world' 'GET greeting' 'INCR hits' 'SETEX token 60 abc' 'TTL token' 'KEYS'
+OK
$hello world
:1
+OK
:60
*3 greeting hits token
$ kill %1
kvserver: stopped with 3 key(s)
```

Requests are single lines: `PING`, `SET key value...`,
`SETEX key seconds value...`, `GET key`, `DEL key`, `INCR key`, `TTL key`
(`-1` without an expiry, `-2` if absent), `KEYS [prefix]`, `SIZE`, and
`QUIT`. Replies are one line each: `+OK`, `$value` (`$` alone for nil),
`:integer`, `*count items...`, or `-ERR message`.

## How it works

- **One event loop** (`kvserver.di`). Each pass calls
  `IO.poll([listener, *client sockets], clients with pending output, 500)`,
  accepts every pending connection (`accept()` on a
  `TCPServer.listen_nonblocking` listener returns `nil` instead of
  blocking), reads what each ready client sent, and writes what each can
  take.
- **Per-connection buffers** (`Connection`). Input accumulates until a full
  line arrives, so a request split across packets or several requests in
  one packet both work. `Socket#write` may be partial, so unsent output
  waits for the socket to be writable.
- **Expiry.** `SETEX` records an expiry as Unix seconds. Reads check it,
  and the loop sweeps expired keys about once a second.
- **Durability** (`lib/store.di`). Every change is appended to the log as a
  JSON array and flushed (`File#flush`); startup replays it. When the log
  holds more than twice the lines the live keys need, it's rewritten to a
  temporary file and swapped in with `File.rename`.
- **Shutdown.** `Signal.trap("TERM", stop)` sets a flag from a nested
  `def`; `IO.poll` wakes on the signal, the loop ends, pending replies are
  sent, and the log is compacted and closed.
- **The protocol** (`lib/protocol.di`) is one `case` over
  `[command, *args]` with Array patterns such as
  `["SETEX", key, seconds, first, *rest]`.

`kvclient.di` is the blocking side: `TCPSocket.connect` with connect and
read timeouts, one request and one reply at a time.

## Test

```sh
bash smoke_test.sh
```

This builds both programs with `diamond build`, starts the server on a
local port (`KV_PORT`, default 18000), and checks commands, expiry, a
request split across packets, pipelined requests, twenty concurrent
clients incrementing one counter, log compaction, SIGTERM shutdown, and
reloading the log after a restart.
