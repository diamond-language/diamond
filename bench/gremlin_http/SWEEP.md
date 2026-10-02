# gremlin_serve thread-count sweep

Recorded 2026-10-02 on the Ryzen 5 Pro 7535U (6 cores / 12 threads), Diamond
0.10.1 `make release` (`-O3 -DNDEBUG -march=native`), gremlin 0.4.0. Run
`python3 bench/gremlin_http/sweep.py` (needs `ab`). The older `run.sh` predates
the move of gremlin to `lib/gremlin.di`/`require_cut` and no longer runs.

Endpoint: ~2000-iteration integer loop, an 8-element array of hashes, and
`JSON.stringify` per request (about 0.77 ms serial including client overhead).
Load: 3 `ab` processes x 32 connections (no keep-alive: gremlin has none),
5 s warmup then 10 s measured, 5 interleaved rounds per thread count.
The host was not idle (browser/music/QEMU idle VM), `ab` shares the cores.

| threads | mean req/s | stdev | mean p99 |
|---:|---:|---:|---:|
| 2  | 2,980 | 11  | 62 ms |
| 4  | 4,645 | 12  | 60 ms |
| 6  | 5,299 | 36  | 73 ms |
| 8  | 5,507 | 40  | 68 ms |
| 10 | 5,623 | 162 | 80 ms |
| 12 | 5,657 | 41  | 73 ms |

Zero failed requests. Throughput scales sub-linearly from the start (per-thread
rate: 1490 req/s at 2 threads, 1160 at 4, 883 at 6, 471 at 12) and plateaus
around 8+ threads, rather than peaking at 4-6.

The load generator is not the ceiling: at 12 threads the server used ~800% CPU
while each `ab` used ~9%, and 6 `ab` clients instead of 3 gave 5,162 (6 threads)
and 5,572 (12 threads) req/s. The cause of the sub-linear scaling is not yet
identified (candidates: per-request kernel TCP setup without keep-alive,
allocator/GC contention across workers, SMT sharing).
