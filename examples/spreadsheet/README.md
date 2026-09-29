# examples/spreadsheet

Loads a sheet of `NAME = FORMULA` lines and prints the evaluated grid.

```text
$ diamond spreadsheet.di testdata/budget.txt
           A         B         C         D
1       1200      rent    1644.5    822.25
2      340.5 groceries   411.125       889
3         89  internet      1200
4         15 streaming        15
```

```text
A1 = 1200
A2 = 340.5
A3 = 89
A4 = 15
B1 = "rent"
B2 = "groceries"
...
C1 = =SUM(A1:A4)
C2 = =AVG(A1:A4)
D1 = =C1 / 2
D2 = =(C1 - A1) * 2
```

A cell holding a bare number or a quoted string is a literal; one starting
with `=` is a formula, parsed and evaluated as an expression over `+ - * /`,
parentheses, other cells by name (`A1`), and `SUM`/`AVG`/`MIN`/`MAX` over a
rectangular range (`SUM(A1:C3)`, not just one row or column).

## Usage

```text
spreadsheet.di SHEETFILE [--cell NAME]
```

Prints the whole grid, or just one cell's value with `--cell`. Exit status
is 0 on success, 64 for a usage error, 65 for a bad sheet (a malformed
line, an unknown function, a circular reference, or division by zero —
each reported with a line number where one applies), and 66 when the file
can't be opened.

## What it shows

- **A sealed AST** (`lib/formula.di`): `Node` and its five kinds. `case`
  over a `Node` with no `else` stops compiling the moment a sixth kind is
  added and not handled — both `formula_show` there and `Sheet#eval_node`
  rely on that.
- **A precedence-climbing parser** (`lib/parser.di`, `lib/lexer.di`), the
  same shape as examples/calc's own, over a narrower grammar: no
  assignment or variables, and a call's only argument shape is a
  `REF:REF` range.
- **Memoized recursion as the dependency graph.** There's no separate
  graph structure — `Sheet#value_of` calling `eval_node` calling
  `value_of` again, for however many cells a formula reaches, *is* the
  walk. `@cache` means a cell that ten others depend on is still only
  evaluated once; `@visiting` is a stack of the names currently being
  resolved, so a reference back to any of them — not just to itself
  directly — raises `CircularReferenceError` naming the whole chain
  (`A1 -> B1 -> A1`), instead of overflowing the native call stack.
- **Cell references as their own little parser**
  (`lib/cellref.di`): `"AA12"` splits into a base-26 column (`A`-`Z`,
  then `AA`-`AZ`, ...) and a row, and back, and a range between two
  corners enumerates every cell in the rectangle regardless of which
  corner is which (`"B2:A1"` and `"A1:B2"` name the same four cells).
- **Union return types** (`Float | String`): a cell is either kind, and
  the evaluator's own `as_number` is where the one deliberate, documented
  coercion lives (a text label used in arithmetic reads as `0`).
- **Exceptions as the error-reporting spine**: `CellRefError`,
  `FormulaError`, and `CircularReferenceError` each carry a message
  straight from where the problem was found, all fed into the CLI's
  own `rescue error: FormulaError | CellRefError | CircularReferenceError`.

## Test

```sh
bash smoke_test.sh
```

Runs the program under the interpreter and as a `diamond build` binary,
and checks the grid, `--cell`, and every error path.
