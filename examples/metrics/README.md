# examples/metrics

A StatsD-style metrics collector over UDP. Applications fire off one-line
datagrams (`requests:1|c`, `queue_depth:7|g`, `latency:12.5|ms`); the server
aggregates them and, on SIGTERM or Ctrl+C, prints what it collected and exits.

```text
$ diamond metrics.di serve 8125 &
listening on udp/8125
$ diamond metrics.di send 8125 'requests:1|c' 'latency:12.5|ms'
ok 1
ok 1
$ diamond metrics.di report 8125
counters
  requests           1.00
timers (ms)
  latency            n=1 min=12.50 mean=12.50 p90=12.50 max=12.50
$ kill -TERM %1
signal received; final totals:
...
```

## Usage

```text
metrics.di serve PORT             # collect until SIGINT/SIGTERM or a !stop datagram
metrics.di send PORT LINE...      # each LINE is one datagram; prints each reply
metrics.di report PORT [--json]   # ask a running server for its aggregates
metrics.di aggregate FILE         # aggregate a file of metric lines, no network
```

Metric types: `|c` counter (summed; `|c|@0.1` scales a 10%-sampled counter
back up), `|g` gauge (last value wins), `|ms` timer (count, min, mean, 90th
percentile, max). A datagram may hold several newline-separated lines and is
all-or-nothing: one bad line rejects the batch. Replies are `ok N` or
`error: ...`. Control datagrams: `!ping`, `!report`, `!text`, `!reset`,
`!stop`. Exit status is 0 on success, 1 when `send` gets an error reply, 64
for a usage error, 65 for a bad line in `aggregate`, and 66 when a file can't
be read or the port can't be bound.

## What it shows

- **UDP request/response.** `UDPSocket.bind(port)` for the server,
  `UDPSocket.open()` for the client, and `send(data, host, port)` /
  `receive(n)` returning `{"data", "host", "port"}`, so the server replies to
  whoever sent each datagram with no connection state at all.
- **`Signal.trap` while blocked in a native call.** The server spends its life
  inside `receive`. A trapped `TERM`/`INT` interrupts that call, runs the
  handler, and the handler prints the totals and calls `exit(0)`. The handler
  is a nested `def` (a closure value) that reads the server's `aggregator`
  local, which is how it sees the live state.
- **A limit worth knowing.** `IO.poll` accepts non-blocking TCP sockets and
  spawned-process streams, not UDP sockets, so a UDP `receive` can't be given
  a timeout. `send` therefore waits forever for a reply if nothing is
  listening, and the smoke test wraps every client call in `timeout`.
- **A `struct` as a parsed record.** `Sample(name, value, kind)`, produced by a
  small hand-written parser that validates names, numbers, types and sample
  rates with `Regexp` and raises `MetricError` naming exactly what was wrong.
- **Percentiles without a library.** Nearest-rank on a sorted Array, with
  `"%.2f".format(x)` for output.
- **Reading a test's bash quirk.** bash starts background jobs with SIGINT
  ignored, and the setting survives `exec`, so the test resets it with
  `(trap - INT TERM; exec ...)` before launching the server.

## Test

```sh
bash smoke_test.sh
```

Compares offline aggregation to a golden file, then starts a real server as a
`diamond build` binary on UDP port 18100 (`METRICS_PORT` overrides it) and
feeds it datagrams, queries and resets it, checks the occupied-port failure,
and shuts it down once by SIGTERM and once by `!stop`, checking the final
totals each time.
