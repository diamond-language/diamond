# examples/tictactoe

Perfect-play tic-tac-toe by exhaustive minimax — small enough (at most 9
levels deep) to search completely, with no need for alpha-beta pruning or
a transposition table.

```text
$ diamond tictactoe.di --best "XX......."
o plays 2 -> o loses with best play
```

X already has two in a row on an otherwise empty board — a well-known
losing position for O no matter what it does, which is exactly what the
search finds: blocking at 2 is O's *least bad* move, and it still loses.

```text
$ diamond tictactoe.di --self
. . .
. . .
. . .

x plays 0
X . .
. . .
. . .
...
draw
```

`--self` plays a complete game with both sides searching every line to
the end — the classic proof that perfect tic-tac-toe always draws, run
rather than just cited.

## Usage

```text
tictactoe.di --self
tictactoe.di --best BOARD      # BOARD is 9 characters: . for empty, X, O
```

`--best` infers whose turn it is from the count of `X`s and `O`s (fewer
marks moves next) and prints that player's best move and what it's worth:
a forced win, a forced loss, or a draw, all assuming both sides play
perfectly from here on. Exit status is 0 on success, 64 for a usage
error, and 65 for a bad board (wrong length, an unexpected character, or
a position that's already won or full).

## What it shows

- **In-place backtracking recursion.** `minimax` doesn't copy the board
  at each call — it writes a move into `board[position]`, recurses, and
  writes `nil` back before trying the next position. The board a caller
  passed in comes back unchanged; every mutation during the search is
  undone on the way back out.
- **One function, two roles, from the same recursion.** `minimax(board,
  player, maximizer)` returns a score that is always from `maximizer`'s
  point of view, whether or not `player` (whose actual turn it is at that
  node) is the same as `maximizer` — the classic minimax trick of picking
  the move that's best for whoever is actually moving, at every level,
  while always scoring from one fixed perspective.
- **A shared top-level constant.** `WIN_LINES` holds the eight winning
  lines and is visible inside `board_winner`.
- **A struct as a plain return-value bundle** (`MoveResult(score, move)`),
  and a union field type (`move: Int | Nil` — no move once the game's
  already over).
- **A deterministic AI as a test fixture.** Both smoke-test checks that
  matter most — the full `--self` game and the forced-win/forced-block
  positions — have exactly one correct answer, which is what makes them
  checkable at all: this is exhaustive search, not a heuristic that could
  reasonably vary.

## Test

```sh
bash smoke_test.sh
```

Runs the program under the interpreter and as a `diamond build` binary,
and checks a full self-play game, two tactical `--best` positions, and
every error path.
