# examples/exact

Linear algebra over exact fractions: solve a system of equations, take a
determinant, or invert a matrix, with every answer a true fraction rather
than a float that is nearly right.

```text
$ diamond exact.di solve testdata/system.txt
x1 = -5/22
x2 = 1/2
x3 = 15/22
```

The classic stress test is the Hilbert matrix, whose entries are
`1/(i+j-1)`. In floating point it is badly conditioned; in fractions its
order-5 inverse is exactly all integers, and the tool proves it by
multiplying back:

```text
$ diamond exact.di inverse testdata/hilbert.txt
[    25    -300     1050    -1400     630 ]
[  -300    4800   -18900    26880  -12600 ]
...
check: matrix * inverse == identity: true
```

## Usage

```text
exact.di solve FILE      # last column is the right-hand side
exact.di det FILE
exact.di inverse FILE
```

`FILE` has one row per line, entries separated by spaces, each an integer or
a fraction such as `3/4`. Blank lines and `#` lines are skipped. Exit status
is 0 on success, 1 when `solve` finds no solution or infinitely many
(it says which, and names the free unknowns), 64 for a usage error, 65 for
a bad matrix (ragged rows, non-square for `det`/`inverse`, singular for
`inverse`, a malformed entry), and 66 when the file can't be read.

## What it shows

- **A value type with the full operator set.** `Fraction` overloads `+ - * /`,
  unary minus (`negate`), `==`, and `<=>` with `Comparable` deriving the
  ordering, and is frozen at construction and kept in lowest terms, so
  equal fractions have equal fields.
- **`[]` and `[]=` as an API.** `matrix[i]` is a row and `matrix[i] = row`
  replaces one (checking the length), so elimination swaps and rewrites rows
  with plain assignment and reads entries as `matrix[i][j]`.
- **Arbitrary-precision `Int`s used for real.** Numerators and denominators
  grow past 64 bits during elimination and simply keep working, including
  through `%` in the GCD. Since Euclid's algorithm is a self-recursive tail
  call, that GCD also runs in constant stack.
- **No coercion.** `fraction + 1` works because the operators accept an `Int`
  on the right; `1 + fraction` would raise `TypeError`, since operator
  dispatch is by the left operand only. The code always keeps the fraction
  on the left.
- **Multiple assignment from a result bundle.** `reduce` returns
  `[matrix, pivots, factor]` and callers unpack it with `[a, b, c] = ...`.
- **A regression test for a real compiler bug.** Writing this example
  turned up a miscompile: `x = nil` at the top of a loop body did not reset
  `x` once the loop assigned it later, which broke the pivot search here.
  It is fixed and covered by
  `tests/cases/register_recycling_nil_literal_in_loop.di`.

## Test

```sh
bash smoke_test.sh
```

Runs each command under the interpreter and as a `diamond build` binary,
compares the solved system and the Hilbert inverse to golden files, and
checks the determinant, the contradictory and underdetermined systems, and
every error path and exit status.
