# examples/life

Conway's Game of Life: loads a pattern and prints it, then each
generation after it.

```text
$ diamond life.di testdata/glider.txt 4
........
........
..#.....
...#....
.###....
........
........
........

Generation 1
........
........
........
.#.#....
..##....
..#.....
........
........
...
```

A glider (the smallest self-propelling pattern) shifts one cell down and
one cell right every 4 generations — after `Generation 4` above, it's
exactly where the starting pattern was, offset by (1, 1).

## Usage

```text
life.di PATTERN_FILE GENERATIONS
```

`PATTERN_FILE` is text: `.` for dead, `#` for alive, one line per row, all
rows the same width. Cells off the edge of the grid always read as dead
— no wraparound — so a pattern that reaches the boundary behaves
differently than it would on an infinite plane, but stays deterministic.
Exit status is 0 on success, 64 for a usage error, 65 for a bad pattern
(uneven row widths or an unexpected character), and 66 when the file
can't be opened.

## What it shows

- **A genuinely two-dimensional structure**: a grid is an `Array` of
  rows, each its own `Array[Bool]` — `grid[row][col]`, not one flat
  array indexed by `row * width + col`. `grid_step` builds the next
  generation as a fresh nested `Array` rather than mutating the one it
  was given, since every cell's next state depends on its neighbors'
  *current* state — updating in place would let an already-stepped
  neighbor leak into a cell that hasn't been computed yet.
- **`each_with_index` over `map`**: building the next grid needs each
  cell's own row/column to look up its neighbors, so the loop pushes
  into a fresh `Array` rather than transforming in place.
- **A whole simulation as one small rule.** `grid_step`'s live-cell/
  dead-cell rule is four lines; everything else here is I/O and
  rendering around it.

## Test

```sh
bash smoke_test.sh
```

Runs the program under the interpreter and as a `diamond build` binary,
and checks a glider over 4 generations and every error path.
