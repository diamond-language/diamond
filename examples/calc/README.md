# examples/calc

An interactive calculator built the way a small language is built: a
tokenizer, a Pratt parser, a sealed syntax tree, an evaluator, and a
simplifier. Errors point at the column that caused them.

```text
$ diamond calc.di
> area = pi * 3 ^ 2
=> 28.274333882308138
> 2 ^ 100
=> 1267650600228229401496703205376
> :tree 2 ^ 3 ^ 2
(2 ^ (3 ^ 2))
> :simplify (y - y) * z + 2 * 3
6
> 2 * (3 + 4
  2 * (3 + 4
            ^ expected ')' but found end of input
```

Each line is an expression, an assignment (`x = 3`), or a command:
`:tree EXPR` shows how the parser grouped it, `:simplify EXPR` shows it
after algebraic simplification, and `:vars` lists defined names. Functions:
`sqrt`, `abs`, `round`, and variadic `min`/`max`. `calc` exits 1 if any line
failed, so it also works as a checker for a file of expressions.

## What it shows

- **A sealed syntax tree.** `Expr` in `lib/ast.di` is `sealed`, with six
  subclasses. `calc_show` and `Evaluator#eval` each `case` over all six;
  forgetting one is a compile error, not a silent `nil`.
- **Object patterns.** `calc_simplify_node` (`lib/evaluator.di`) states
  each rewrite as a pattern over the tree, such as
  `BinOp{op: "*", left: Num{value: 0}}`. Comma-separated alternatives bind
  the same name (`other`) so one branch covers `x + 0`, `0 + x`, `x * 1`,
  and the rest; an `if` guard handles `a - a`.
- **Array patterns with rest bindings.** The parser's `parse_prefix`
  matches `[token.kind(), token.text()]` against `[:op, "-"]` and
  `[:name, name]`. The command dispatcher in `calc.di` matches split words
  against `[":tree", _, *_]`.
- **A struct and Symbols.** `Token` is a one-line `struct` whose `kind` is
  a Symbol (`:number`, `:name`, `:op`, `:end`).
- **An exception hierarchy.** `ParseError` and `EvalError` extend
  `CalcError`, which adds a `column` to `StandardError` and calls
  `super(message)`. `Evaluator#call` rescues a builtin's `ArgumentError`
  and re-raises it as an `EvalError` at the call's column.
- **Closures in a table.** `calc_builtins` returns nested `def`s in a
  Hash, called later with a spread (`function(*args)`). `min` and `max`
  are variadic (`def calc_min(first, *rest)`).
- **Numbers.** Integer results stay `Int`, and `Int` promotes past 64 bits
  on its own, so `2 ^ 100` is exact. `Evaluator#int_pow` squares its way
  up as a self-recursive tail call, which Diamond runs in constant stack
  space. `-7 % 3` is `2`: `%` is floored, as in Ruby.
- **Visibility.** The parser's and evaluator's helpers sit under
  `private`.

## Test

```sh
bash smoke_test.sh
```

This replays `testdata/session.txt` through the interpreter and through a
`diamond build` binary, and compares both transcripts with
`testdata/session.expected`.
