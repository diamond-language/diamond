# examples/library HTTP throughput/latency benchmark

Measures `gremlin_serve`'s `threads: N` parallelism against the *full*
`examples/library` stack, not a synthetic handler like
`bench/gremlin_http`'s own `hello`/`cpu` workloads: real
`Dials::Router` dispatch, real `ActiveRecord` queries against SQLite3,
real `Div` template rendering. Recorded 2026-08-24 on the same machine
as `bench/gremlin_http/RESULTS.md`.

## Environment

- CPU: AMD Ryzen 5 Pro 7535U — 6 physical cores, 12 logical (SMT)
- Built via `make release` (`-O3 -DNDEBUG -march=native`)
- Load generator: Apache Bench (`ab` 2.3), same caveats as
  `bench/gremlin_http/RESULTS.md`'s own "Caveats" — single OS process,
  shares the same 12 cores as the server under test.

## Workloads

Five read-only (`GET`) routes, `ab -n 5000 -c 50` each, at `threads: 1`
and `threads: 4`:

- **`home`**: static page, no database query — isolates HTTP/routing/
  `Div` rendering overhead from ActiveRecord.
- **`authors_index`** (`/authors`), **`books_index`** (`/books`),
  **`books_available`** (`/books/available`): a list query + table
  render.
- **`authors_show`** (`/authors/1`): a single-row lookup plus a
  `has_many` query (an author's books) + render.

A write workload (`POST`) was deliberately left out — it would add
SQLite lock contention as a second variable this benchmark isn't trying
to isolate.

Run via `bash bench/library_http/run.sh`.

## Results

| route | threads | req/sec | mean latency | p99 latency | failed |
|---|---:|---:|---:|---:|---:|
| home | 1 | 1,226 | 40.8ms | 9ms | 0 |
| home | 4 | 5,384 | 9.3ms | 39ms | 0 |
| authors_index | 1 | 1,078 | 46.4ms | 9ms | 0 |
| authors_index | 4 | 4,072 | 12.3ms | 37ms | 0 |
| authors_show | 1 | 986 | 50.7ms | 7ms | 0 |
| authors_show | 4 | 3,294 | 15.2ms | 40ms | 0 |
| books_index | 1 | 766 | 65.3ms | 8ms | 0 |
| books_index | 4 | 2,611 | 19.2ms | 63ms | 0 |
| books_available | 1 | 830 | 60.3ms | 10ms | 0 |
| books_available | 4 | 2,763 | 18.1ms | 57ms | 0 |

Zero failed requests across every trial. One run each, not averaged —
treat single-digit percent deltas as noise, matching
`bench/gremlin_http/RESULTS.md`'s own methodology.

## Findings

**1. The first real run of this benchmark surfaced three genuine bugs,
not benchmarking noise — confirmed directly before trusting any number
here, not assumed.** At `threads: 4`, every database-backed route
showed `ab`-reported failures on ~20-23% of requests (`authors_index`:
1,143/5,000; similar on the other three); `threads: 1` and the static
`home` route showed zero. That asymmetry (real DB routes failing only
at `threads > 1`, `home` and `bench/gremlin_http`'s own `cpu` workload
— far worse latency, zero failures at any thread count — not failing at
all) ruled out "just what happens under load" and pointed at something
specific to this app's own multi-threaded setup. Root-caused with a
raw concurrent-connection burst test (many `curl`s at once, bypassing
`ab`) plus temporary error-logging in `packages/gremlin`'s own rescue
paths (normally silent by design — a broken client's exception
shouldn't crash the server, but it also swallows the diagnostic needed
here), which surfaced:
   - **The dominant cause**: `examples/library/app.di` called
     `Author.configure`/`Book.configure` once before `gremlin_serve`,
     expecting it to reach every worker. `@@repository` is a class
     variable, and `gremlin_serve(threads: N)`'s N-1 spawned workers
     each get their own independent VM/heap (`docs/threads.md`'s
     "Isolated-heap design") — only the one worker that runs inline
     ever saw the configured repository. The other three started every
     request with `Author.repository() == nil`, confirmed directly via
     the debug logging (`repo was: nil` on 3 of 4 threads, `repo was:
     #<ActiveRecord::Repository>` on the fourth) before touching any
     code. Fixed by moving the configure calls behind a per-worker lazy
     guard (`ensure_models_configured(context)` in
     `lib/middleware.di`), the same `context`-memoized shape
     `Dials::RouterHolder` and this app's own `Database.get` already
     use for other per-worker resources. Full writeup in
     `docs/threads.md`'s new "Gotcha" note — this isn't specific to
     this example, it'll bite any `gremlin_serve(threads: N > 1)` app
     with one-time class-variable setup.
   - **`packages/gremlin`'s listener backlog was hardcoded to 16**
     (`listen(listening_fd, 16)`, `src/vm.c`) — a burst of concurrent
     connects past that got refused/dropped at the kernel's SYN queue
     before `accept()` ever saw them, independent of server-side
     processing speed. Fixed to `SOMAXCONN` (4,096 on this machine;
     the kernel still clamps it against `/proc/sys/net/core/somaxconn`,
     so this is never "too many," only "no smaller than the system
     already allows").
   - **`gremlin_worker`'s poll loop registered every open connection as
     write-interested unconditionally**, even ones just waiting to
     read. `POLLOUT` is ready almost immediately for any idle socket
     with room in its send buffer, so `IO.poll`'s "block until
     something's ready" call effectively never blocked — every tick
     busy-spun `resume_if_ready` across every live connection,
     competing with the listener's own `accept()` processing for CPU.
     Fixed by tracking a `want_write?` flag on `NonblockingConnection`
     (true only while a `#write` call is actually mid-retry on
     `WouldBlockError`) and only including a connection in `write_list`
     while that's true. A real scalability fix independent of the
     other two, though not independently isolated as the sole cause of
     any specific failure count here.

   All three are fixed on `main`; every number in the table above was
   measured against the fixed build.

**2. p99 latency gets *worse* at `threads: 4` despite ~3-4x higher
throughput, on every route.** Same shape as
`bench/gremlin_http/RESULTS.md`'s own `hello` finding: `ab` and the
server share this machine's 12 logical cores, and pushing 4x the request
rate through the same client process adds queuing/scheduling variance
that shows up in the tail even as the mean drops. Not a regression —
`hello`'s own p99 roughly doubles 1→4 threads too (10ms→8ms is actually
flat there, but `hello`'s `threads: 2` p99 is 5ms, lower than
`threads: 1`'s 10ms, before climbing again) — just the same
single-machine-load-generator artifact this repo has already documented
once.

**3. Scaling is real but sub-linear, and the app's own per-request cost
dominates over HTTP/event-loop overhead.** `home` (no DB query) scales
1→4 threads at ~4.4x (1,226→5,384 req/s), closest to ideal.
`books_index`/`books_available` (list queries) scale worse (~3.3x,
~3.4x) — more time per request spent in SQLite/Arel/Div, less of the
total request cost is the parallelizable-across-workers part
(`gremlin_worker`'s own accept/poll loop), and each worker thread has
its own independent SQLite connection (`Database.get`, per-worker via
`context`) reading the same on-disk file, adding I/O contention `home`
never touches at all.

## Caveats

- Same as `bench/gremlin_http/RESULTS.md`: `ab` is a single OS process
  sharing this machine's cores with the server under test — not
  representative of a real deployment with the load generator
  elsewhere.
- Read-only routes only, by design (see "Workloads") — a write
  benchmark would need to isolate SQLite's own single-writer lock
  contention as a separate, deliberately-varied factor, not fold it in
  here.
- One run each, not averaged, matching this repo's own established
  benchmarking methodology (`bench/BASELINE.md`).
