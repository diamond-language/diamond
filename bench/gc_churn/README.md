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

## Results: pre-generational baseline

All runs on this machine (12 logical / 6 physical cores), debug build,
single-threaded, wall time via `/usr/bin/time -f %es`. This section
predates the generational collector (see "Results: generational GC + card
marking" below for the current numbers) -- kept as the baseline the
generational work was measured against.

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

## Results: generational GC + card marking

Same machine, same debug build shape, now with the generational collector
(see `docs/internal/gc-generational-design.md`) at its tuned nursery threshold
(`minor_gc_threshold_bytes` = 1MiB). `DIAMOND_TRACE_GC=1` now reports major
and minor collections separately.

**Live-set-size sweep** (`session_churn.di`, churn held fixed at 200,000
iterations):

| live set | major coll. | major GC time | minor coll. | minor GC time | total GC time | wall time | GC share |
|---------:|------------:|---------------:|------------:|---------------:|---------------:|----------:|---------:|
|    1,000 |         211 |          1.62s |       3,378 |           0.36s |          1.98s |    11.45s |    17.3% |
|    5,000 |          55 |          2.07s |       3,417 |           0.37s |          2.44s |    12.05s |    20.2% |
|   10,000 |          36 |          2.07s |       3,437 |           0.38s |          2.45s |    12.24s |    20.0% |
|   20,000 |          27 |          2.05s |       3,446 |           0.40s |          2.45s |    12.44s |    19.7% |
|   40,000 |          23 |          2.06s |       3,446 |           0.44s |          2.50s |    12.98s |    19.3% |

The headline result: minor-collection count and cost are now essentially
flat across a 40x live-set range (~3,400 collections, ~0.4s total, ~0.11-
0.13ms each) -- unlike the old collector, where *every* collection walked
the whole live set and individual pause cost grew 7x (15ms -> 103ms) across
this same sweep. Major-collection cost still scales with live-set size
(that pass still walks everything, by design), but there are far fewer of
them relative to the total collection count, so the *typical* pause a
running program experiences is now bounded regardless of live-set size --
the actual goal this work targeted.

**Churn-volume sweep** (`session_churn.di`, live set held fixed at 20,000):

| iterations | major coll. | major GC time | minor coll. | minor GC time | total GC time | wall time | GC share |
|-----------:|------------:|---------------:|------------:|---------------:|---------------:|----------:|---------:|
|     50,000 |          19 |          0.41s |         859 |           0.10s |          0.51s |     3.48s |    14.6% |
|    100,000 |          22 |          1.02s |       1,722 |           0.20s |          1.22s |     6.55s |    18.6% |
|    200,000 |          27 |          2.05s |       3,446 |           0.40s |          2.45s |    12.52s |    19.6% |
|    400,000 |          37 |          4.13s |       6,896 |           0.81s |          4.94s |    24.46s |    20.2% |

**Control** (`pure_churn.di`, no live set at all -- nothing survives to be
promoted, so this only ever triggers major collections, same as before the
generational change):

| iterations | major coll. | major GC time | wall time | GC share |
|-----------:|------------:|---------------:|----------:|---------:|
|     50,000 |      18,754 |          0.17s |     2.28s |     7.5% |
|    100,000 |      37,504 |          0.34s |     4.48s |     7.5% |
|    200,000 |      75,004 |          0.66s |     8.79s |     7.5% |
|    400,000 |     150,004 |          1.38s |    17.70s |     7.8% |

Total GC time is now close to the pre-generational baseline's (e.g. 2.45s
vs. ~2.5s at live_set=20,000/200,000 iterations) -- a dramatic recovery
from the first (reverted) generational attempt's whole-object-remembering
regression, which cost 64s of minor-collection time alone at this same
configuration (see `docs/internal/gc-generational-design.md`'s own comparison
table). Wall time is still higher than the pre-generational baseline
(~12.4s vs. ~7.35s at this configuration) -- attributed to the per-object
bookkeeping now present on every allocation/mutation path plus every minor
survivor being promoted immediately (no survival threshold); not chased
further here since bounding pause length, not aggregate wall time, was the
stated goal.

## Reading

The analysis below describes the pre-generational collector's own
behavior (the problem this whole benchmark was built to characterize) --
see "Results: generational GC + card marking" above for how the shipped
fix changed these numbers.

Two separate, independent effects show up here, and they matter for
different reasons:

1. **Per-collection pause cost scales with live-set size, not with churn.**
   At fixed churn (200k iterations), growing the live set 40x (1,000 →
   40,000 sessions) makes each individual collection ~7x more expensive
   (15ms → 103ms) -- because today's collector (`diamond_vm_collect_impl`,
   `src/vm.c`) is a plain stop-the-world mark-and-sweep: every collection
   re-marks and re-walks the *entire* live heap, including the large,
   already-stable session cache that hasn't actually changed shape since
   the last cycle. This is the concrete mechanism
   `docs/internal/gc-generational-design.md`'s nursery/write-barrier
   design targets: a minor collection would only need to scan young
   objects plus whatever
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
