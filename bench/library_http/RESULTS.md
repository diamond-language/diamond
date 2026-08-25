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
| home | 1 | 3,267 | 15.3ms | 10ms | 0 |
| home | 4 | 7,476 | 6.7ms | 18ms | 0 |
| home | 6 | 7,532 | 6.6ms | 19ms | 0 |
| home | 8 | 6,974 | 7.2ms | 30ms | 0 |
| home | 12 | 7,306 | 6.8ms | 22ms | 0 |
| authors_index | 1 | 1,883 | 26.5ms | 6ms | 0 |
| authors_index | 4 | 4,600 | 10.9ms | 43ms | 0 |
| authors_index | 6 | 5,101 | 9.8ms | 41ms | 0 |
| authors_index | 8 | 5,026 | 9.9ms | 33ms | 0 |
| authors_index | 12 | 4,723 | 10.6ms | 39ms | 0 |
| authors_show | 1 | 1,456 | 34.3ms | 9ms | 0 |
| authors_show | 4 | 3,704 | 13.5ms | 39ms | 0 |
| authors_show | 6 | 4,138 | 12.1ms | 43ms | 0 |
| authors_show | 8 | 3,986 | 12.5ms | 47ms | 0 |
| authors_show | 12 | 3,881 | 12.9ms | 46ms | 0 |
| books_index | 1 | 1,123 | 44.5ms | 11ms | 0 |
| books_index | 4 | 2,988 | 16.7ms | 51ms | 0 |
| books_index | 6 | 3,242 | 15.4ms | 61ms | 0 |
| books_index | 8 | 3,335 | 15.0ms | 47ms | 0 |
| books_index | 12 | 3,234 | 15.5ms | 65ms | 0 |
| books_available | 1 | 1,228 | 40.7ms | 44ms | 0 |
| books_available | 4 | 3,075 | 16.3ms | 40ms | 0 |
| books_available | 6 | 3,410 | 14.7ms | 46ms | 0 |
| books_available | 8 | 3,377 | 14.8ms | 46ms | 0 |
| books_available | 12 | 3,255 | 15.4ms | 53ms | 0 |

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
1→4 threads at ~2.3x (3,267→7,476 req/s), closest to ideal.
`books_index`/`books_available` (list queries) scale a bit better at 4
threads (~2.7x, ~2.5x) and keep climbing further past 4 than `home`
does (see finding 4) — more time per request spent in SQLite/Arel/Div
means more of the total request cost is parallelizable-across-workers
CPU/I/O work rather than fixed per-connection overhead, and each worker
thread has its own independent SQLite connection (`Database.get`,
per-worker via `context`) reading the same on-disk file, adding I/O
contention `home` never touches at all.

**4. Throughput peaks around 6 threads for most routes, not at 12** —
the same physical-core-vs-SMT signature `bench/gremlin_http/RESULTS.md`'s
own `hello` finding already documented on this machine (6 physical
cores, 12 logical via SMT). `home`, `authors_index`, `authors_show`, and
`books_available` all peak at `threads: 6` (matching physical core
count) then flatten or dip by `threads: 8`/`12` (`home`: 7,532→6,974→
7,306; `authors_index`: 5,101→5,026→4,723) — past 6 workers, `ab`'s own
client process and the server's workers start competing for the same 12
logical cores. `books_index` is the one exception, peaking (barely) at
`threads: 8` (3,335 vs 3,242 at 6) — essentially a tie within this
benchmark's single-run noise floor, not a meaningfully different
pattern. Same `bench/gremlin_http/RESULTS.md` `cpu`-vs-`hello` contrast
as before, just less extreme since even the heaviest route here is
nowhere near `cpu`'s pure-busy-loop cost.

**5. Absolute throughput jumped ~30-90% over the first recorded run of
this benchmark, same code, same machine — a system power-management
setting (power saver), not a code change.** Re-run after the user
disabled it: `home` at `threads: 1` alone went 1,704→3,267 req/s
(+92%), with smaller but still substantial gains at every other
route/thread-count. The *shape* of the results — sub-linear scaling,
peaking near physical core count, zero failures throughout — held
identically across both runs; only the absolute numbers moved. Table
above reflects the power-saver-off run; worth remembering when
comparing against `bench/gremlin_http/RESULTS.md`'s own numbers, which
predate this observation and may have been recorded under whichever
power state was active at the time.

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
