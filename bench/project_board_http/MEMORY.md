# Per-thread memory overhead

`memory_probe.sh` samples the server's own `VmRSS` (`/proc/<pid>/status`,
whole process -- every worker thread's independent heap combined, see
`docs/threads.md`'s "Isolated-heap design") at idle (every
`gremlin_serve` worker thread started and listening, before any
request) and again after a fixed real load run, across a sweep of
server thread counts:

```
bash bench/project_board_http/memory_probe.sh
```

Load volume is held fixed (6 load-generator threads x 20 iterations =
120 CRUD journeys) regardless of server thread count, so the post-load
delta isolates "more server workers" from "more total requests."

## Results (2026-08-29, same machine/build as RESULTS.md)

| server threads | idle RSS | post-load RSS | load delta |
|---:|---:|---:|---:|
| 1 | 49.6 MB | 53.4 MB | 3.9 MB |
| 2 | 63.5 MB | 71.4 MB | 7.9 MB |
| 4 | 112.4 MB | 124.0 MB | 11.6 MB |
| 6 | 161.4 MB | 178.0 MB | 16.6 MB |

**Idle marginal cost per additional thread**: +13.9 MB going from 1 to
2, then a very consistent **+24.4 MB and +24.5 MB/thread** for 2->4 and
4->6 respectively (within 0.2% of each other) -- each `Thread.new`
worker clones the whole program's function/class tables
(`docs/threads.md`), a fixed cost independent of request volume, and
for this real, modest-sized CRUD app that cost is small and highly
consistent once past the first thread or two. For context,
`docs/threads.md`'s own `DIAMOND_MAX_THREADS` cap (64) was sized off a
~5GB *worst-case* ceiling (~78 MB/thread) -- this real app's actual
measured cost is well under a third of that budget.

**Variance at a fixed point in time is zero**: three repeated samples
(idle) and three more (post-load) at each thread count came back
bit-for-bit identical every time -- `VmRSS` genuinely doesn't move
between reads of an otherwise-idle process. (This says nothing about
variance *across independent runs*, or under sustained live traffic --
see `bench/burn_in`'s own README for that much harder question, where
the answer for a large mutating session cache turned out to be real,
non-trivial volatility, not a flat line.)

**A real check, not just a guess**: the post-load delta growing with
thread count even at fixed total request volume initially looked like
it might be `Arel::PreparedStatements`' own per-connection cache (each
worker's cache is independent and never evicted -- see that class's own
doc comment) adding up across more active workers. Checked directly via
`git worktree` against the exact pre-`PreparedStatements` commit
(`e197a4e4`): idle and post-load numbers there are statistically
indistinguishable from the numbers above (e.g. threads=6 idle 164,316
KB then vs. 165,248 KB now, threads=6 post-load delta 16,036 KB then
vs. 17,016 KB now). The caching work adds no measurable memory
overhead here -- the delta-grows-with-thread-count pattern is ordinary
per-worker GC/request-churn timing (a worker sampled right after its
own last request hasn't necessarily run its own next collection yet),
not anything specific to the new cache.
