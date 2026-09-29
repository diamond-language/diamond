# examples/brainfuck

A Brainfuck interpreter whose main loop is a self-recursive tail call, and a
companion `depth.di` that measures which recursion shapes Diamond optimizes.

```text
$ diamond brainfuck.di testdata/hello.bf --steps
Hello World!
906 instructions
```

Every Brainfuck instruction is one more call of the same function, so even
that hello world is 906 calls deep, and `testdata/busy.bf` (two nested
255-iteration loops) is 326,765. Ordinary recursion in Diamond stops at 95
calls. This program never gets near it, because a function's own tail call
reuses its frame.

```text
$ diamond depth.di
shape                                 depth   result
self tail call                       100000   ok
non-tail: 1 + f(n - 1)               100000   stack overflow
tail call inside begin/rescue        100000   stack overflow
mutual tail calls (ping/pong)        100000   stack overflow
non-tail, within the limit               80   ok
```

## Usage

```text
brainfuck.di FILE [--input TEXT] [--limit N] [--steps]
depth.di [N]
```

`--input` supplies the bytes `,` reads (0 once they run out), `--limit N`
stops a program after N instructions (default 10,000,000), and `--steps`
prints the instruction count to stderr. Cells wrap at 256 and the tape is
30,000 cells. Exit status is 0 on success, 64 for a usage error, 65 for a
program error (unmatched bracket, the pointer leaving the tape, the step
limit), and 66 when the file can't be read.

## What it shows

- **A tail call as the loop.** `execute` ends with
  `return execute(machine, code, jumps, next_pc, steps + 1, limit)`: the
  whole value of a `return`, in a plain function, outside any `begin`. The
  work of each instruction is pushed into `Machine#apply`, so the recursive
  call stays that simple.
- **What does not qualify, measured.** `depth.di` runs the same 100,000-deep
  count four ways. Only the self tail call survives; a non-tail recursion,
  a tail call inside `begin/rescue`, and mutual recursion between two
  functions all hit the depth limit. None of those is an error in the
  language, just ordinary recursion.
- **The catch.** A qualifying tail call that never terminates now runs
  forever instead of overflowing the stack, so `execute` keeps an explicit
  step limit (`+[]` is `testdata/forever.bf`). Without one, the old
  "it'll crash eventually" safety net is gone.
- **Mutable state next to an immutable loop.** The tape, pointer and output
  live in one `Machine` object, which is why the loop itself needs only a
  program counter and a step count.
- **A precomputed jump table.** `jump_table` matches every `[` with its `]`
  in one pass with a stack, so each jump is an array lookup.
- **`Machine#apply` returns the next pc**, so control flow is data: an
  ordinary instruction returns `pc + 1`, a taken jump returns its partner
  plus one.

## Test

```sh
bash smoke_test.sh
```

Runs the interpreter and `depth.di` interpreted and as `diamond build`
binaries: output and instruction counts of the sample programs, the step
limit, each error message and exit status, and the depth table.
