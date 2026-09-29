# examples/leaderboard

Replays a log of `name score` events into a ranked board, announcing rank
changes as they happen.

```text
$ diamond leaderboard.di testdata/season1.txt --top 4
ann joins with 50
bob joins with 70
bob takes the lead from ann
...
eve takes the lead from dee

season1.txt: 7 players
  1  eve          500  (gold)
  2  ann          120  (silver)
  3  dee          120  (bronze)
  4  fay           90
     ... and 3 more

total 950, mean 135, best eve, lowest ge
above the mean: eve
...
```

## Usage

```text
leaderboard.di FILE [--top N] [--quiet] [--versus FILE2]
```

A player's board score is their best; only a personal best changes the board.
`--quiet` skips the running commentary, `--top N` sets how many rows print
(default 5), and `--versus FILE2` replays a second log and says which board's
combined score is higher. Exit status is 0 on success, 64 for a usage error,
65 for a malformed log line (with its file and line number), and 66 when a
file can't be read.

## What it shows

- **`include Enumerable` from one method.** `Leaderboard#each` is the only
  iteration code; `map`, `min`, `max`, `take`, `partition`, `group_by`,
  `each_slice`, and even `.lazy()` all come from it. (Enumerable's `sum`
  takes no block, so scores are `map`ped out first.)
- **A mixin with state.** `Observable` is a `module` whose methods are copied
  into the class that includes it, keeping its listener list in the host's
  own `@listeners`, created on first use so the host's `initialize` needs no
  cooperation. Listeners are plain closures called as `listener(event, details)`;
  `nested_listener` shows that a nested `def` is a closure value.
- **A module as a namespace.** `Ranking.medal` and `Ranking.tier` are
  `def self.` functions, callable only as `Ranking.name(...)`.
- **`protected`.** `Leaderboard#outscores?` reads another board's hidden
  `total`, legal because both are Leaderboards; from outside, `board.total()`
  raises `TypeError`. `private` hides `ranked`, the sorted view everything
  else reads.
- **`Comparable`, and its one trap.** `Entry` orders by score, then name, and
  gets `<`, `max`, `sort` from `<=>`. But Comparable's derived `==` passes
  whatever it is compared to straight into `<=>`, so `entry == nil` would
  reach a method whose parameter insists on an `Entry` and fail. `Entry`
  therefore defines its own `==`, as `examples/ledger`'s `Money` does. `Entry`
  is a `struct`, and a struct's hand-written `def ==` replaces the generated
  field-by-field one.
- **Structural `case`.** `describe` matches `[event, details]` against
  `[:joined, [name, score]]` and friends, binding names out of the nested
  array.

## Test

```sh
bash smoke_test.sh
```

Runs the program interpreted and as a `diamond build` binary and compares the
full replay and the `--quiet` form to golden files, checks `--versus` from
both sides, and checks every error path and exit status.
