# examples/vending

A coin-operated vending machine driven by a script of JSON commands: coins,
selections, refunds, restocking, and a keyed service mode. It is a state
machine written as nested `case` expressions, and it exists to put Diamond's
pattern matching through its paces.

```text
$ diamond vending.di testdata/session.txt
2: insert coins first
3: credit $1.00
4: dispensed A1 for $0.75
4: returned $0.25: 25c
5: credit $0.25
6: rejected a 3c coin
...
17: wrong service key
18: service mode on
18: returned $1.00: 100c
```

## Usage

```text
vending.di SCRIPT
```

One JSON object per line: `{"cmd": "coin", "cents": 25}`,
`{"cmd": "select", "slot": "A1"}`, `{"cmd": "refund"}` (or `"cancel"`),
`{"cmd": "restock", "slot": "A1", "count": 3, "price": 75}`, and
`{"cmd": "service", "key": "1234"}`. Blank lines and `#` lines are skipped.
A malformed command is reported to stderr with its line number and skipped;
the run then exits 1. Exit status is 0 for a clean run, 64 for a usage error,
and 66 when the script can't be read.

## What it shows

- **Exhaustive `case` over sealed hierarchies.** `State` and `Event` are
  `sealed`, and the outer `case state` and each inner `case event` name every
  kind. Add a `Kick < Event` and the program stops compiling at each `case`
  that forgot it: `case is not exhaustive ...; missing: Kick`.
- **Object patterns.** `when HasCredit{cents: credit}` checks the class and
  binds a public reader's result in one step; an empty `Service{}` is
  "any Service".
- **Hash patterns with `**rest`.** The parser matches
  `{"cmd": "coin", "cents": cents, **rest}` and a guard requires `rest` be
  empty, so a misspelled extra key is an error, not silently ignored. A
  final `{"cmd": name, **rest}` clause turns a known command with bad
  arguments into a precise message, and an unknown one into another.
- **Guards, alternatives, and fall-through.** `if !coin_values().include?(cents)`
  vetoes a `Coin{cents: cents}` match, which then falls to the next `when`;
  `{"cmd": "refund", **rest}, {"cmd": "cancel", **rest}` are alternatives
  binding the same names, sharing one body.
- **`^pin`.** `Service{key: ^key}` compares against the machine's own key
  instead of rebinding `key`.
- **Multiple assignment and rest.** `[price, count] = @stock[slot]`,
  `[_, before] = ...`, and `[state, messages] = machine.step(state, event)`
  unpack results.
- **Exhaustiveness has rules worth knowing.** Comma-separated *object
  patterns* in one `when` (`Coin{}, Select{}`) are not credited as covering
  those classes, but comma-separated class names (`Coin, Select`) are, so the
  out-of-service branch spells the latter.
- **`protected`.** `Machine#maintenance` and the rest of the private machinery
  are only callable from inside the class hierarchy, while `step` is the one
  public transition.

## Test

```sh
bash smoke_test.sh
```

Runs the program interpreted and as a `diamond build` binary, compares a long
session and a malformed-command script to golden output, and checks the error
messages, exit statuses, and every usage path.
