# examples/generators

Fibers used four ways: as generators of infinite sequences, as lazy
pipelines built from those generators, as two-way coroutines, and as tasks
under a small cooperative scheduler.

```text
$ diamond generators.di
== infinite generators
naturals:  0 1 2 3 4 5 6 7
fibonacci: 0 1 1 2 3 5 8 13 21 34 55 89
primes:    2 3 5 7 11 13 17 19 23 29 31 37 41 43 47
fib(150):  9969216677189303386214405760200
...
== scheduler
t=0 kettle: heating
t=0 toaster: slice 1 in
t=0 ticker: 1
t=1 ticker: 2
t=2 toaster: slice 2 in
t=2 ticker: 3
t=2 smoke alarm failed: false alarm
t=3 kettle: boiled
...
```

`expected.txt` has the complete output.

## What it shows

- **Generators from blocks.** `Generator.new() do ... end`
  (`lib/generator.di`) stores its block through an `&block` parameter. Each
  `each` or `first(n)` call runs the block in a fresh `Fiber` and resumes it
  once per value; the block hands values out with `Fiber.yield`. Because
  values are produced on demand, `naturals()`, `fibonacci()`, and `primes()`
  never end, and `first(n)` stops after `n`.
- **Enumerable from `each`.** `Generator` includes `Enumerable` and defines
  `each(callback)`, so a finite generator gets `to_a`, `sum`, `select`,
  `group_by`, and the rest.
- **Lazy pipelines.** `mapping`, `selecting`, and `taking_while` each return
  a new generator whose block pulls from the previous one. A `Fiber.yield`
  inside a nested `each` callback suspends the outer generator's fiber, with
  the inner one still paused where it was. The demo compares this with the
  built-in `.lazy()` on a `Range`.
- **Coroutines.** The averager receives each sample as the result of
  `Fiber.yield` and sends the running average back out. The tokenizer keeps
  its place in a string between resumes, finishes with `:done`, and shows
  `status()`, `alive?()`, and the `FiberError` from resuming a finished
  fiber.
- **Closures as fiber bodies.** `make_averager` and `make_tokenizer` return
  nested `def`s. `Fiber.new` needs a zero-argument callable, so
  `make_tokenizer` captures its `text` argument instead of taking it as a
  parameter.
- **A scheduler.** `Scheduler` (`lib/scheduler.di`) keeps one fiber per task
  on a simulated clock. Tasks yield `[:sleep, ticks]` or `:pass`, and `step`
  matches the request with an array pattern and an `if` guard. A task that
  raises is rescued at `resume`, logged, and dropped without disturbing the
  others. Because nothing is preempted, the trace is identical every run.
- **Big integers.** The 150th Fibonacci number needs more than 64 bits, and
  Diamond's `Int` promotes on its own.

Fibers are cooperative and share one OS thread; for parallel work see
[`docs/threads.md`](../../docs/threads.md).

## Test

```sh
bash smoke_test.sh
```

This runs the demo under the interpreter and as a `diamond build` binary and
compares both with `expected.txt`.
