# gremlin_serve multi-thread scaling: investigation plan

Status: **plan, nothing here is confirmed yet.** Starting evidence is
`bench/gremlin_http/SWEEP.md` (recorded 2026-10-02, Diamond 0.10.1,
gremlin 0.4.0, Ryzen 5 Pro 7535U, 6 cores / 12 threads).

## What was observed

Requests per second for a small mixed-work endpoint (about 0.77 ms serial):

| threads | 2 | 4 | 6 | 8 | 10 | 12 |
|---|---:|---:|---:|---:|---:|---:|
| req/s | 2,980 | 4,645 | 5,299 | 5,507 | 5,623 | 5,657 |
| per-thread req/s | 1,490 | 1,160 | 883 | 688 | 562 | 471 |

1. Scaling is sub-linear from the first doubling (2→4 threads gave +56%, not
   +100%) and flat from about 8 threads. The expected peak at 4–6 threads did
   not appear; 12 threads was marginally best.
2. The load generator is not the ceiling. At 12 threads the server used about
   800% CPU and each `ab` about 9%; six `ab` clients instead of three changed
   nothing material.
3. CPU is being spent without producing requests: roughly 1.4 ms of CPU per
   request at 12 threads versus 0.77 ms serial.
4. The server's resident memory was about 893 MB at 12 threads (unexplained).
5. The older single-run `bench/gremlin_http/RESULTS.md` (2026-08-17, a
   different build) is a useful clue: its pure-CPU workload scaled about 4.2x
   from 1 to 6 threads, while its near-empty `hello` workload peaked at 4
   threads and fell by 12. If that still holds, compute scales and the loss is
   in per-request fixed costs (connection setup, allocation, response
   building), not in running Diamond code in parallel.

## Hypotheses

Each is something that could produce observation 1–3. They are not mutually
exclusive.

- **H1 Clock/power limit.** A 15 W-class mobile part drops per-core frequency
  as more cores load. This alone could explain a falling per-thread rate and
  is entirely outside Diamond.
- **H2 SMT and shared resources.** Threads 7–12 are siblings of busy cores,
  so they add little regardless of software. Expected for any CPU-bound
  server; it bounds what "good" looks like.
- **H3 Kernel TCP cost.** gremlin has no keep-alive, so every request is a
  full accept/connect/close on loopback. That kernel work may dominate and may
  not scale (shared socket-hash and accept paths), and it is billed to the
  server's CPU.
- **H4 Allocator contention.** Workers have independent Diamond heaps but may
  share glibc malloc arenas and locks for underlying allocation.
- **H5 Runtime-global locks or shared state.** A mutex or atomic in the
  runtime shared by every VM (symbol or class tables, caches, interning, GC
  coordination) would serialise workers.
- **H6 GC behaviour.** Collection frequency or cost per worker could grow with
  concurrency; the large RSS may be the same cause (or a leak).
- **H7 Uneven connection distribution.** `SO_REUSEPORT` hashes connections
  across listeners, which can leave some workers idle and others queued.
- **H8 Harness noise.** Other host load (browser, music, an idle QEMU VM) and
  `ab` itself sharing the cores.

## Plan, cheapest discriminating experiments first

Each step says what result would make us stop, narrow, or continue. Record
numbers in `SWEEP.md` (or a sibling file) as we go, with the date and the
build used.

**Step 0: make the harness trustworthy.** Quit other desktop apps, note the
CPU governor, and pin `ab` to two cores while the server uses the rest
(`taskset`), so the generator stops competing for server cores. Re-run the
2/6/12 points. If the plateau moves, part of it was H8 and the rest of the
plan runs on the better harness. Do this before anything else.

**Step 1: frequency and topology (H1, H2).** Sample per-core frequency
(`/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq`) during 2/6/12-thread
runs, and read the SMT sibling layout (`lscpu -e`). Run 6 threads pinned to six
distinct physical cores versus six that include siblings. Normalise per-thread
rate by measured frequency. If frequency drops explain most of the per-thread
decline, the software ceiling is much closer to the line than it looks, and we
should stop treating the plateau as a bug. If not, continue.

**Step 2: where the CPU goes (H3).** For 2/6/12 threads record user versus
system CPU time, voluntary and involuntary context switches
(`/proc/<pid>/status`, `pidstat`-style sampling) and per-thread CPU (`top -H`),
which also checks H7 (are all workers equally busy?). A large and growing
system share points at the kernel path; a high user share points at Diamond.

**Step 3: separate fixed cost from compute (H3, H4, H5).** Re-run the sweep
with three endpoints: `hello` (fixed tiny response), the current mixed-work
endpoint, and a pure arithmetic loop. If compute scales and `hello` does not,
the problem is in per-request overhead, and Steps 4–6 are where to look. If
all three flatten together, suspect H1/H2 or a global limit.

**Step 4: a no-Diamond baseline (H3).** Write a small C epoll accept/respond
server using `SO_REUSEPORT` with N threads and no keep-alive, and run the same
`ab` load. If it plateaus at a similar request rate, the ceiling is the
loopback and harness cost, not gremlin. If it scales much further, the gap is
ours and its size tells us how much is recoverable.

**Step 5: allocator A/B (H4).** Cheap environment-only tests first
(`MALLOC_ARENA_MAX`, glibc tunables), then an `LD_PRELOAD` of a
thread-caching allocator if one is installable. A real difference means
allocation is shared; look at how workers obtain heap memory.

**Step 6: GC and memory (H6).** Run with `DIAMOND_TRACE_GC` at 2/6/12 threads
and compare collection counts and pause time per request. Run a long (about 60
second) fixed-thread load and watch RSS: growing means a leak or unbounded
heap growth, flat but large means thresholds. Check whether the large RSS is
per-worker heap duplication.

**Step 7: find shared locks (H5), only if steps 2–6 do not explain it.**
Poor-man's profiling: attach `gdb` to the 12-thread server and sample all
thread backtraces repeatedly (about 50 samples), then count how many sit in
futex or mutex waits and in which functions. Grep the runtime for
process-global mutexes and shared caches to cross-check. `perf` is not
installed (installing it would need root); if you are happy to install it, it
replaces most of this step. Beware two known pitfalls from earlier profiling
in this project: callgrind overstates `memset` (counted per byte), and the
debug build is `-O0`, so profile only `make release`, and confirm every
conclusion with wall-clock, not just instruction counts.

## Decision points

- After Step 1: if frequency/SMT explain most of the gap, write that up in
  `SWEEP.md`, adjust expectations, and stop unless Step 4 says otherwise.
- After Step 4: the C baseline defines the achievable ceiling. Our target is
  the fraction of that ceiling gremlin should reach, to be set once we see it,
  not before.
- Any fix lands on its own branch with the sweep re-run before and after, and
  a regression-level check (throughput and p99 at 2/6/12 threads) so the win
  is measured, not argued. Do not claim a cause until a change to that cause
  moves the numbers.

## Out of scope for now

Keep-alive support, pipelining, and the HTTP parser are separate features. If
Step 3 or 4 shows the no-keep-alive connection cost dominates, that becomes a
separate design question rather than part of this investigation.
