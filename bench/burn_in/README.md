# bench/burn_in

The long-running, multi-threaded workload `docs/roadmap.md`'s
"Generational or incremental GC" entry and `docs/gc-generational-design.md`
both call for: a real `gremlin_serve(..., threads: 4)` HTTP server (see
`packages/gremlin`) under sustained `ab` load, standing in for the
fiber-per-connection production shape that entry names as the actual
motivating case for a generational collector -- not started until there
was a workload to validate against, which is what this directory is for.

## Running it

```
make release
bash bench/burn_in/run.sh [duration_seconds] [concurrency]
```

Defaults: 300s total, concurrency 20, in 15s `ab` batches. Each batch line
reports the server's RSS (`VmRSS`, the whole process -- all 4 worker
threads' independent heaps combined, see `docs/threads.md`) alongside that
batch's throughput and tail latency from `ab`.

## Reading the output

`server.di`'s handler is deliberately request-scoped only: it builds and
discards a Hash/Array/String per request, nothing persists across
requests (`threads: N` requires a zero-capture `Callable`, so there's no
closing over mutable state even if this wanted to simulate a longer-lived
working set -- see the comment at the top of `server.di`). That shape
exercises steady young-object churn against a small, stable live set --
exactly what a nursery is meant to reclaim cheaply, but *not* the
large-live-set case the roadmap's GC entry is actually worried about.

A representative run (120s, concurrency 20, release build):

```
batch      rss_kb    req_per_sec    mean_ms     p95_ms     p99_ms
1          354396       13994.70      1.429          2          3
2          354636       14704.00      1.360          2          3
...
8          354464       13906.85      1.438          2          3

RSS: start=247136KB end=354464KB over 8 batches
RSS: first 3-batch avg=354368KB, last 3-batch avg=354464KB
```

RSS jumps once in the first batch (three `Thread.new` calls each clone
the ~83MB ambient `DiamondProgram`, see `docs/threads.md`, plus the
current collector's `next_gc` doubling threshold ratcheting up from its
2048-byte starting point to fit the real live set) and then goes
completely flat -- no further growth, no throughput or p99 degradation,
across the rest of the run. That's the *current* mark-sweep collector
doing fine against *this* workload shape, which is expected: this
handler never grows the live set, so there's nothing for a generational
collector to save here yet.

**What this does not yet test**: a large, mostly-stable live set (a
session cache, connection registry, or similar) growing alongside the
request churn -- the actual case the roadmap flags as motivating. If a
flat RSS/latency line here is being used to argue *against* generational
GC, that argument doesn't hold: this harness would need a handler that
builds real persistent state before its results speak to that question.
Extending `server.di` with one is the natural next step whenever someone
picks the GC item back up.

## A bug this found

Standing this up the first time surfaced a real, 100%-reproducible hang
in `gremlin_serve(..., threads: N)` for any `N > 1`: `Thread.new`'s
return value was discarded for each spawned worker, so the handle was
GC-unreachable the instant it was created, and `free_thread`'s
block-join guarantee (src/vm.c) then blocked the spawning thread forever
joining a worker whose event loop is deliberately infinite. Fixed in
`packages/gremlin/gremlin.di` by rooting every spawned `Thread` in an
Array that outlives the server. See that commit for the full writeup --
worth knowing about if `run.sh` ever seems to hang again with `ab`
reporting lost/timed-out requests, since that's exactly this bug's
symptom.
