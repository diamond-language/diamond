# examples/parallel

Real parallelism on OS threads: fanning work out with `Thread.new`, a
worker pool and a pipeline wired together with `Channel`s, what thread
isolation means in practice, and what happens when a worker fails.

```text
$ diamond parallel.di --time
== fan-out
  range 1: 3711 runs for 238 steps
  ...
longest Collatz sequence below 24001: 23529, 282 steps
  serial 3.12s, 4 threads 1.13s (2.8x)
...
== failure
join re-raised: worker rejected -1
succeeded on attempt 3
restarts: 2, last error: uncaught exception: RuntimeError: flaked on attempt 2
```

`expected.txt` has the complete output. `--time` adds timings on stderr;
stdout is the same on every run, whatever order the threads finish in.
(Timings above are from a debug build; `make release` is roughly 10x
faster. `diamond --version` says which one you have.)

## What it shows

1. **Fan-out.** Four `Thread.new(collatz_longest, low, high)` calls split a
   CPU-bound search into ranges; `join()` returns each result, and
   `max_by` picks the winner.
2. **A worker pool.** A feeder thread sends jobs into a bounded `Channel`
   and closes it. Three workers `receive` until they get `nil` (closed and
   drained) and send results back on a second channel. Results arrive in
   any order, so the main thread merges them by key before printing.
3. **A pipeline.** Three stage threads connected by channels: numbers →
   squares → digit sums. Each stage closes its output when its input runs
   dry, so shutdown flows down the chain. `try_receive` on an empty channel
   raises `WouldBlockError` instead of waiting.
4. **Isolation.** A worker gets a deep copy of its arguments: its `push`
   doesn't reach the parent's array, and its `@@count` class variable
   starts from scratch. A capturing closure can't be sent to a thread at
   all (`TypeError`).
5. **Failure.** An exception in a worker is re-raised by `join()`. A
   `Supervisor` restarts a crashing worker in a fresh heap; the worker
   learns which attempt it is on from a channel of tickets, since a channel
   is the one value shared by reference across attempts.

Everything a thread runs lives in `lib/work.di` as a plain top-level
`def` with no captures, which is what `Thread.new` and
`Supervisor#add_child` require.

Channel parameters are annotated `Channel`, so passing anything else to a
worker fails at the call.

## Test

```sh
bash smoke_test.sh
```

This runs the demo under the interpreter and as a `diamond build` binary,
checks both against `expected.txt`, and checks that `--time` changes only
stderr.
