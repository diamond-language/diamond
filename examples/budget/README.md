# examples/budget

What a wall-clock budget does to a program that blocks, and what each way of
misbehaving looks like from the outside. A host runs five small guests as child
processes, each under `DIAMOND_MAX_WALL_MILLISECONDS=300`, and reports how each
one ended.

```text
$ diamond host.d6 diamond
quick
  finishes inside the budget; nothing trips
  exited normally
  > sum 499500
  within 3s: true
blocked receive
  Channel#receive with no sender is bounded by the budget
  exited normally
  > receive was cut short: resource limit exceeded
  within 3s: true
...
rescue then block
  blocking again after the error hits the grace limit
  stopped by the hard limit
  > rescued once
  ! wall-clock budget exhausted: the program kept running after its ResourceLimitError
  within 3s: true
```

`expected.txt` has the complete output, which is the same on every run.

## What it shows

1. **A blocked wait is bounded.** A `Channel#receive` nobody will answer, or an
   `IO.poll` with no timeout, sleeps in the kernel where a per-instruction clock
   check never runs. The budget bounds the wait itself, so the guest gets a
   rescuable `ResourceLimitError` after about 300 ms instead of hanging. The
   same holds for `Channel.select`, `Channel.select_send`, and `Channel#send` on a full
   channel.
2. **Rescuing is how you finish cleanly.** In `guests/cleanup.d6` the error is
   rescued, `ensure` runs, and the process exits with status 0.
3. **The budget fires once.** `guests/rescue_then_block.d6` rescues the error
   and then blocks again. The wall-clock budget is off after it fires, but a
   grace allowance of one second (counted from the deadline, not from the
   rescue) is all the program gets. After that the process is stopped with an
   error no `rescue` can match, and the exit status is 70.
4. **Budgets are per process.** `DIAMOND_MAX_WALL_MILLISECONDS` is read once at
   startup, so the host sets it in each child's environment with `env`. The
   `plugins` example uses the same approach for the instruction budget and the
   sandbox allow-list.

The host classifies a child only by what an operator can see: its exit status,
its output, and the first line of its error.

## Run it

```sh
make debug
bash examples/budget/smoke_test.sh
```

The smoke test runs the host interpreted and as a `diamond build` binary and
compares both with `expected.txt`.
