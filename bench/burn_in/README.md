# bench/burn_in

The long-running, multi-threaded workload motivated by
`docs/internal/gc-generational-design.md`: a real `gremlin_serve(..., threads: 4)` HTTP server (see
`packages/gremlin`) under sustained `ab` load, standing in for the
fiber-per-connection production shape that entry names as the actual
motivating case for a generational collector -- not started until there
was a workload to validate against, which is what this directory is for.

## Running it

```
make release
bash bench/burn_in/run.sh [duration_seconds] [concurrency]
```

For anything beyond `server.di`'s documented 20000-session-per-worker
config -- a bigger live set, specifically -- use `run_hard.sh` instead,
never `run.sh` directly:

```
bash bench/burn_in/run_hard.sh [duration_seconds] [concurrency] [server_script]
# BURN_IN_PORT and BURN_IN_RSS_CAP_KB env vars override the defaults
```

`run_hard.sh` carries an active watchdog -- polling the server's RSS
every second, independent of `ab`'s own batch boundaries, and `kill -9`-
ing the server the instant RSS crosses `BURN_IN_RSS_CAP_KB` (default
5,500,000, i.e. ~5.5GB) -- that `run.sh` doesn't have. This exists
because an earlier, unsupervised attempt at pushing this benchmark's
live-set size (3x the documented config, no hard cap, "scale back if it
looks dangerous" as the only guidance) consumed all RAM and most of
swap on the single machine this project runs on and crashed it. Always
choose the cap from actual `free -h` headroom at launch time, not a
guess -- `run_hard.sh` itself refuses to start if free memory doesn't
leave real margin above the configured cap. See "A follow-up push"
below for what this was built for and what it found.

Defaults: 300s total, concurrency 20. `run.sh` first sends a small
calibration batch to measure this workload's actual achievable
throughput, then sizes every real batch's request count to target
roughly 15s of genuine load each -- deliberately **not** using `ab`'s own
`-t` (time limit) flag, which turned out to silently imply `-n 50000`
internally regardless of any larger `-n` given alongside it (`man ab`;
confirmed empirically -- `-n 100000000 -t 180` still stopped dead at
exactly 50000 requests in ~6s). Relying on `-t` here would have meant
each "batch" was a few seconds of real load followed by the server
sitting idle for the rest of the batch window while the duration counter
kept ticking -- the exact kind of thing this harness exists to catch,
just aimed at itself the first time it was built. Each batch line reports
the server's RSS (`VmRSS`, the whole process -- all 4 worker threads'
independent heaps combined, see `docs/threads.md`) alongside that batch's
actual duration, throughput, and tail latency from `ab`.

## Reading the output

Two allocation shapes run on every request (see `server.di`'s own
comments):

- **Request-scoped churn** (`build_payload`): a Hash/Array built and
  discarded per request -- exactly what a nursery is meant to reclaim
  cheaply.
- **A persistent, bounded per-worker session cache** (`touch_session`,
  backed by `context`): up to 20000 session records per worker, each
  replaced wholesale on every touch. This is the large, mostly-stable
  live set the roadmap's GC entry is actually worried about, and it's
  also under continuous mutation -- every touch is an old-generation
  Hash (`sessions`) gaining a fresh young value, exactly the old-to-young
  write `docs/internal/gc-generational-design.md`'s write barrier section is
  written against.

Getting the session cache to exist at all took a real detour: `handler`
must be a zero-capture `Callable` under `threads > 1` (`Thread.new`
hard-rejects any closure that captures locals), and Diamond has no
class-variable or other static-storage mechanism a zero-capture function
could reach by name instead -- confirmed directly (`@@x` doesn't parse;
`def self.x` methods have no per-class field storage). `packages/gremlin`
now solves this generally: `handler` takes a second argument, `context`,
a per-worker `Hash` created by `gremlin_worker` itself *after* the Thread
boundary is already crossed, so it never needs to cross that boundary in
the first place. See `packages/gremlin/README.md`'s "Per-worker context"
section for the full mechanism.

### A representative long run (600s requested, concurrency 20, release build)

34 batches, ~615s of actual continuous load (`run.sh` targets `-n` per
batch rather than wall-clock time -- see "Running it" above -- so real
elapsed slightly overshoots the request):

```
burn-in: calibrated ~8257 req/s -> 123863 requests/batch (target ~15s/batch)
batch      rss_kb    req_per_sec    mean_ms     p95_ms     p99_ms   failed    batch_s
1         4830152        8116.29      2.464          2          5        0     15.261
2         4802288        7166.90      2.791          2          2        0     17.283
...
17        5092880        6490.95      3.081          2          3        0     19.082
...
34        4591256        7134.86      2.803          2          3        0     17.360

RSS: start=430000KB end=4591256KB over 34 batches
RSS: first 3-batch avg=4679350KB, last 3-batch avg=4452560KB
```

RSS climbs during the first couple batches (every worker's 20000-slot
session cache filling for the first time) and then stays in a 4.2-5.1GB
band for the rest of the run -- bouncing, not trending: the first-3-batch
average is actually *higher* than the last-3-batch average. Throughput
settles from an initial ~8100 req/s down to a steady ~6500-7000 req/s
once the cache is warm (each touch now does a real hash update instead
of a fresh insert), and p99 latency is flat at 2-3ms for essentially the
entire run. Zero failed requests across all 34 batches. So: over 10+
minutes of continuous mutation at the full session-cache cap, the
*current* mark-sweep collector shows no memory growth, no throughput
decay, and no latency decay against this workload.

**What this shows and doesn't show**: RSS stabilizes noticeably higher
than the request-scoped-only baseline (a few hundred MB there vs. several
GB here for a session cache that's logically maybe a few hundred MB of
data). That gap is real and worth being precise about: it's consistent
with per-object `malloc` overhead and the current collector's
non-compacting, individually-`malloc`'d design (`docs/internal/gc-generational-design.md`
explicitly scopes out moving/compacting as a non-goal, for unrelated
reasons -- see that doc's "structural fact" section), not necessarily
something a generational collector specifically would fix. What
generational GC targets is the *CPU cost* of re-walking this whole live
set on every collection, not its memory footprint -- this harness can
show whether that re-walk cost is visible in throughput/latency (a flat
line here says it currently isn't, at this live-set size and this
request rate), but a flat memory line isn't evidence either way for that
question.

### A follow-up push (found the 4.2-5.1GB band understates real variance)

The natural next step from the run above -- push the live-set size
harder to see if a re-walk cost becomes visible -- ran into a real
incident first: an unsupervised attempt at 60000 sessions/worker (3x
this file's documented config), with only "scale back if it looks
dangerous" as guidance and no active memory ceiling, consumed all RAM
and most of swap on the single machine this project runs on and
crashed it. `run_hard.sh` (see "Running it" above) exists because of
that -- a real, second-by-second RSS watchdog with a hard, pre-checked-
against-`free`-headroom kill threshold, so a repeat of that specific
failure mode is no longer possible regardless of what live-set size or
duration gets tried next.

Two watchdog-protected confirmation runs followed, at the *original*
20000/worker config (not yet a bigger one) to establish how repeatable
the 600s run above actually is:

1. Current `main`, 1800s requested, concurrency 20: RSS climbed
   3.8 -> 4.4 -> 4.9 -> 5.4GB across the first four ~11s batches and
   crossed the 5.5GB watchdog cap before batch 6, well inside the first
   minute.
2. The *exact commit* the 600s/4.2-5.1GB numbers above were recorded at
   (`e9e1afb`, checked out via `git worktree` and rebuilt), same config,
   same cap: RSS swung 4.1 -> 4.2 -> 4.4 -> 5.2 -> 4.9 -> 4.5 -> 4.7GB
   across seven batches and also crossed 5.5GB shortly after.

Both runs ended via the watchdog firing as designed -- neither came
close to threatening the machine. But the result itself matters: **old
code and current code show the same volatile behavior**, which rules
out a regression in anything landed between those two points, but also
rules out "flat 4.2-5.1GB band" as an accurate one-line summary of this
workload's real memory behavior -- that description was one run's own
trajectory, not evidence of a tight, reliable steady state. What's
still genuinely open: whether this volatility is bounded noise around a
higher-than-documented plateau, or a slow climb the original run's own
trajectory happened not to show -- telling those apart needs a run
meaningfully longer than a few minutes at this same, already-known-safe
live-set size, which hasn't been attempted yet. Do that (via
`run_hard.sh`, with its cap chosen from real headroom at launch time)
before pushing live-set size any further.

## Bugs this found

Standing this up surfaced two real, unrelated bugs -- both fixed, both
worth knowing about if this harness ever misbehaves again:

- **`gremlin_serve(..., threads: N > 1)` hung deterministically under
  load.** `Thread.new`'s return value was discarded for each spawned
  worker, so the handle was GC-unreachable the instant it was created,
  and `free_thread`'s block-join guarantee (`src/vm.c`) then blocked the
  spawning thread forever joining a worker whose event loop is
  deliberately infinite. Fixed in `packages/gremlin/gremlin.di` by
  rooting every spawned `Thread` in an Array that outlives the server.
  Symptom if this regresses: `ab` reports lost/timed-out requests, and
  `ss -tn` shows connections stuck in `CLOSE-WAIT` with unread data.
- **`ab -t` silently caps total requests at 50000**, ignoring any larger
  explicit `-n` -- see "Running it" above. Symptom if this regresses:
  batch throughput numbers look fine but `batch_s` is much smaller than
  the batch's intended duration, and the total run finishes far faster
  than `duration_seconds` would suggest.
