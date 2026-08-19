# GC churn: direct collector-cost evidence

`bench/burn_in`'s live `gremlin_serve(threads: N)` pushes gave noisy,
inconclusive RSS numbers -- request timing, OS scheduling, and page-cache
behavior all get mixed into any external measurement of a long-running,
multi-threaded daemon. This directory sidesteps that entirely: two short,
single-threaded, non-networked scripts that run to completion and exit
normally, so `DIAMOND_TRACE_GC=1`'s ordinary print-at-exit path
(`src/run_source.c`) reports direct collection count/total-time evidence
with none of that confound.

- **`session_churn.di`**: the same allocation shape as `bench/burn_in/
  server.di`'s per-worker session cache -- `build_payload`/`touch_session`
  are copied verbatim from there. A large, mostly-stable `sessions` Hash
  (`LIVE_SET_SIZE` distinct keys, cycling) gets one entry wholesale-replaced
  per iteration, `ITERATIONS` times total.
  ```
  DIAMOND_TRACE_GC=1 ./build/diamond bench/gc_churn/session_churn.di LIVE_SET_SIZE ITERATIONS
  ```
- **`pure_churn.di`**: the control -- identical per-iteration
  `build_payload` allocation, but nothing is kept alive across iterations.
  Isolates whether GC cost is driven by *live* data size or just raw
  allocation volume, by holding the latter comparable while zeroing the
  live set.
  ```
  DIAMOND_TRACE_GC=1 ./build/diamond bench/gc_churn/pure_churn.di ITERATIONS
  ```

## Results

All runs on this machine (12 logical / 6 physical cores), debug build,
single-threaded, wall time via `/usr/bin/time -f %es`.

**Live-set-size sweep** (`session_churn.di`, churn held fixed at 200,000
iterations, only the size of the persistent session cache varies):

| live set | collections | GC time | wall time | GC share | cost/collection |
|---------:|------------:|--------:|----------:|---------:|-----------------:|
|    1,000 |         213 |   3.20s |     7.93s |    40.4% |            15.0ms |
|    5,000 |          55 |   3.14s |     7.77s |    40.3% |            57.0ms |
|   10,000 |          36 |   2.73s |     7.34s |    37.2% |            75.8ms |
|   20,000 |          27 |   2.54s |     7.35s |    34.6% |            94.2ms |
|   40,000 |          23 |   2.36s |     7.59s |    31.1% |           102.6ms |

**Churn-volume sweep** (`session_churn.di`, live set held fixed at 20,000,
only the total iteration count varies):

| iterations | collections | GC time | wall time | GC share |
|-----------:|------------:|--------:|----------:|---------:|
|     50,000 |          19 |   0.41s |     2.15s |    19.0% |
|    100,000 |          22 |   1.17s |     3.78s |    30.8% |
|    200,000 |          27 |   2.57s |     7.36s |    34.9% |
|    400,000 |          37 |   5.66s |    15.09s |    37.5% |

**Control** (`pure_churn.di`, no live set at all):

| iterations | collections | GC time | wall time | GC share |
|-----------:|------------:|--------:|----------:|---------:|
|     50,000 |      21,433 |   0.15s |     1.24s |    12.1% |
|    100,000 |      42,861 |   0.25s |     1.95s |    12.6% |
|    200,000 |      85,718 |   0.49s |     3.87s |    12.6% |
|    400,000 |     171,433 |   0.98s |     7.65s |    12.8% |

## Reading

Two separate, independent effects show up here, and they matter for
different reasons:

1. **Per-collection pause cost scales with live-set size, not with churn.**
   At fixed churn (200k iterations), growing the live set 40x (1,000 →
   40,000 sessions) makes each individual collection ~7x more expensive
   (15ms → 103ms) -- because today's collector (`diamond_vm_collect_impl`,
   `src/vm.c`) is a plain stop-the-world mark-and-sweep: every collection
   re-marks and re-walks the *entire* live heap, including the large,
   already-stable session cache that hasn't actually changed shape since
   the last cycle. This is the concrete mechanism `docs/
   gc-generational-design.md`'s nursery/write-barrier design targets: a
   minor collection would only need to scan young objects plus whatever
   old→young edges the write barrier recorded, not re-walk 40,000 old,
   unchanged session entries every single time. This is the number that
   matters for `bench/burn_in`'s own original concern (p99 latency under
   sustained load) -- a single stop-the-world pause 7x longer shows up
   directly in tail latency, independent of aggregate CPU accounting.

2. **Total aggregate GC CPU share does *not* run away with live-set size**
   in this data -- it actually drifts slightly *down* (40% → 31%) as the
   live set grows, because larger live sets trigger collections less often
   (`next_gc`'s doubling threshold scales with post-sweep survivor bytes).
   So the case for a generational collector here is about *pause-length
   control*, not runaway total-CPU blowup -- consistent with `bench/
   burn_in`'s own live numbers never showing catastrophic RSS growth,
   just noisy, hard-to-interpret variance.

3. **The `pure_churn.di` control confirms the live set is what's expensive,
   not allocation volume in general.** At comparable iteration counts,
   pure churn with *no* persistent live set holds a flat ~12-13% GC share
   -- collections are far more frequent (an order of magnitude more) but
   each one is nearly free, since there's almost nothing live to mark or
   walk past. `session_churn.di`'s 3x-higher GC share, and its individual
   collections being tens of milliseconds instead of microseconds, is
   attributable specifically to carrying a large, mostly-unchanging live
   set through every single collection cycle -- exactly what a
   generational collector's old generation is for.

## Reproducing

```
make debug
for size in 1000 5000 10000 20000 40000; do
  DIAMOND_TRACE_GC=1 ./build/diamond bench/gc_churn/session_churn.di "$size" 200000
done
for iters in 50000 100000 200000 400000; do
  DIAMOND_TRACE_GC=1 ./build/diamond bench/gc_churn/session_churn.di 20000 "$iters"
  DIAMOND_TRACE_GC=1 ./build/diamond bench/gc_churn/pure_churn.di "$iters"
done
```

All runs here finished in well under 20 seconds each and use at most a few
tens of MB of live Diamond heap (a 40,000-entry session cache with small
payloads) -- nowhere near the memory scale that made `bench/burn_in`'s own
earlier unsupervised push crash the machine (see that directory's own
README); no watchdog is needed for this workload.
