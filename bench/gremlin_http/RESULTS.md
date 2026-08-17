# gremlin_serve HTTP throughput/latency benchmark

Measures `gremlin_serve`'s `threads: N` parallelism (see
`packages/gremlin/gremlin.di`) under real concurrent HTTP load — one
`SO_REUSEPORT` listener per worker thread, each its own independent
`DiamondVm`/heap. Recorded 2026-08-17 on the same machine as
`bench/BASELINE.md`.

## Environment

- CPU: AMD Ryzen 5 Pro 7535U — **6 physical cores, 12 logical (SMT)**
- Built via `make release` (`-O3 -DNDEBUG -march=native`)
- Load generator: Apache Bench (`ab` 2.3) — no `wrk`/`hey` available in
  this environment. `ab` itself runs on the same 12-core machine as the
  server under test, competing for the same cores at high thread
  counts (see "Findings" below).

## Workloads

- **`hello`**: fixed tiny response, no per-request compute — measures
  raw request-handling overhead (HTTP parse/write, fiber scheduling,
  syscalls), not application logic. `ab -n 20000 -c 100`.
- **`cpu`**: a ~3ms busy loop (50,000 iterations of plain `Int`
  arithmetic) per request — CPU-bound work a fiber yield can't help
  with, so this is the workload that actually shows whether `threads:
  N` buys real multi-core parallelism versus one worker. `ab -n 3000 -c
  50`.

Run via `bash bench/gremlin_http/run.sh` (rebuild `make release`
first for realistic numbers — this was run against that build, not
`make debug`).

## Results

| workload | threads | req/sec  | mean latency | p99 latency | failed |
|---|---:|---:|---:|---:|---:|
| hello | 1  | 2,569  | 38.9ms | 10ms | 0 |
| hello | 2  | 10,548 | 9.5ms  | 5ms  | 0 |
| hello | 4  | 14,177 | 7.1ms  | 8ms  | 0 |
| hello | 6  | 13,054 | 7.7ms  | 14ms | 0 |
| hello | 12 | 8,883  | 11.3ms | 21ms | 0 |
| cpu | 1  | 377   | 132.8ms | 7,884ms | 0 |
| cpu | 2  | 608   | 82.3ms  | 1,080ms | 0 |
| cpu | 4  | 1,190 | 42.0ms  | 370ms   | 0 |
| cpu | 6  | 1,594 | 31.4ms  | 362ms   | 0 |
| cpu | 12 | 1,855 | 27.0ms  | 395ms   | 0 |

Zero failed requests across every trial. One run each, not averaged —
treat single-digit percent deltas as noise, matching `bench/BASELINE.md`'s
own methodology.

## Findings

**1. `cpu` scales with *physical* core count (6), not logical thread
count (12).** Going 1→6 threads is a clean ~4.2x throughput gain
(377→1,594 req/s) with mean latency dropping proportionally
(132.8ms→31.4ms) — real parallel work landing on genuinely separate
cores. Going 6→12 threads adds only another ~16% (1,594→1,855 req/s),
consistent with the extra 6 threads being SMT siblings sharing
execution resources with the first 6, not independent cores. This is
the textbook SMT signature for CPU-bound work, and it's clearly
visible in `gremlin_serve`'s own scaling curve — the `threads:`
parameter genuinely does put work on separate cores, up to the
hardware's real core count.

**2. `hello` throughput peaks at 4 threads (14,177 req/s) and gets
*worse* past 6.** Unlike `cpu`, this workload is dominated by syscall/
event-loop overhead per request, not CPU work a second core actually
helps with — and past ~4-6 gremlin worker threads, those workers start
competing with `ab`'s own client process for the same 12 logical
cores, so added workers cost more in scheduling contention than they
return in parallelism. `threads: 12` for a lightweight handler on this
machine is a net loss (8,883 req/s) versus `threads: 4` — more workers
isn't free, and the right number depends on how much real CPU work is
actually in the handler, not just "more is better."

**3. Zero failed requests at every thread count, including the
`cpu`-workload's worst latency (p99 ≈ 7.9s at `threads: 1`, backed up
behind 50 concurrent 3ms×N requests queuing on one single-threaded
worker).** No crashes, no dropped connections, no corruption under
sustained concurrent load — `gremlin_serve`'s fiber+worker model holds
up correctly even under a workload it isn't well-suited for
(all-CPU, no I/O to yield on) at low thread counts, it's just slow
there, exactly as expected.

## Caveats

- `ab` is a single OS process; at high req/sec it may itself become
  the bottleneck rather than the server (a real risk for the `hello`
  results, given finding #2 above) — a multi-threaded load generator
  (`wrk`, `hey`) on a separate machine would give a cleaner read on
  `gremlin_serve`'s own ceiling, decoupled from the client's.
- Every trial ran on this one machine with `ab` and the server
  sharing the same 12 cores — not representative of a real deployment
  where the load generator is elsewhere.
