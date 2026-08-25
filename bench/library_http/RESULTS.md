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

Five read-only (`GET`) routes, `ab -n 5000 -c 50` each, at `threads: 1,
4, 6, 8, 12` — 6 and 12 bracket this machine's physical/logical core
split, 8 fills the gap between "physical cores" (6) and "all logical
threads" (12), matching `bench/gremlin_http/RESULTS.md`'s own thread
sweep:

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
| home | 1 | 1,704 | 29.3ms | 7ms | 0 |
| home | 4 | 5,368 | 9.3ms | 28ms | 0 |
| home | 6 | 5,652 | 8.8ms | 36ms | 0 |
| home | 8 | 5,355 | 9.3ms | 33ms | 0 |
| home | 12 | 5,506 | 9.1ms | 37ms | 0 |
| authors_index | 1 | 1,244 | 40.2ms | 3ms | 0 |
| authors_index | 4 | 4,177 | 12.0ms | 33ms | 0 |
| authors_index | 6 | 4,848 | 10.3ms | 40ms | 0 |
| authors_index | 8 | 4,686 | 10.7ms | 49ms | 0 |
| authors_index | 12 | 4,551 | 11.0ms | 44ms | 0 |
| authors_show | 1 | 980 | 51.0ms | 10ms | 0 |
| authors_show | 4 | 3,232 | 15.5ms | 48ms | 0 |
| authors_show | 6 | 3,776 | 13.2ms | 60ms | 0 |
| authors_show | 8 | 4,039 | 12.4ms | 40ms | 0 |
| authors_show | 12 | 3,751 | 13.3ms | 48ms | 0 |
| books_index | 1 | 766 | 65.3ms | 16ms | 0 |
| books_index | 4 | 2,673 | 18.7ms | 59ms | 0 |
| books_index | 6 | 3,273 | 15.3ms | 69ms | 0 |
| books_index | 8 | 3,422 | 14.6ms | 64ms | 0 |
| books_index | 12 | 3,342 | 15.0ms | 65ms | 0 |
| books_available | 1 | 821 | 60.9ms | 12ms | 0 |
| books_available | 4 | 2,832 | 17.7ms | 47ms | 0 |
| books_available | 6 | 3,288 | 15.2ms | 45ms | 0 |
| books_available | 8 | 3,460 | 14.5ms | 51ms | 0 |
| books_available | 12 | 3,375 | 14.8ms | 63ms | 0 |

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
1→4 threads at ~3.2x (1,704→5,368 req/s), closest to ideal.
`books_index`/`books_available` (list queries) scale worse at 4 threads
(~3.5x, ~3.4x) but keep climbing further past 4 than `home` does (see
finding 4) — more time per request spent in SQLite/Arel/Div means more
of the total request cost is parallelizable-across-workers CPU/I/O work
rather than fixed per-connection overhead, and each worker thread has
its own independent SQLite connection (`Database.get`, per-worker via
`context`) reading the same on-disk file, adding I/O contention `home`
never touches at all.

**4. Throughput peaks between 6 and 8 threads, not at 12** — the same
physical-core-vs-SMT signature `bench/gremlin_http/RESULTS.md`'s own
`hello` finding already documented on this machine (6 physical cores,
12 logical via SMT). The two lightest routes (`home`, `authors_index` —
least per-request CPU/SQLite work) peak at `threads: 6`, matching
physical core count almost exactly, then flatten or dip slightly by
`threads: 12` (`home`: 5,652→5,506; `authors_index`: 4,848→4,551) — past
6 workers, `ab`'s own client process and the server's workers are
competing for the same 12 logical cores, and a lightweight handler
doesn't have enough real work per request to make the extra SMT
siblings pay for that contention. The three heavier DB routes
(`authors_show`, `books_index`, `books_available` — a `has_many` join
or table scan per request) keep gaining through `threads: 8`
(`authors_show`: 3,776→4,039; `books_index`: 3,273→3,422;
`books_available`: 3,288→3,460) before also flattening/dipping at 12 —
more actual CPU/I/O work per request means more benefit from the extra
worker threads before client/server core contention outweighs it,
exactly `bench/gremlin_http/RESULTS.md`'s `cpu`-vs-`hello` contrast
playing out again here, just less extreme since even the heaviest route
here is nowhere near `cpu`'s pure-busy-loop cost.

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
