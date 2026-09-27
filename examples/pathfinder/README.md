# examples/pathfinder

Shortest routes across an ASCII map, three ways: breadth-first search,
Dijkstra's algorithm, and A*. The map is printed with the route drawn as `*`.

```text
$ diamond pathfinder.di --algo astar --stats testdata/mud.txt
##############
#S...........#
#*9999999999.#
#*9********9.#
#*9*######*9.#
#***#G*****9.#
##############
astar: 22 steps, cost 22, looked at 47 cells
$ diamond pathfinder.di --algo bfs --stats testdata/mud.txt
##############
#S*********..#
#.99999999*9.#
#.9.......*9.#
#.9.######*9.#
#...#G*****9.#
##############
bfs: 18 steps, cost 26, looked at 53 cells
```

Ground costs what its digit says (`.` is 1, `9` is 9). Breadth-first search
counts steps and ignores cost, so it wades through the mud for a shorter but
dearer route. Dijkstra and A* find the cheapest one; A* gets there having
looked at fewer cells, because its straight-line-distance guess steers it toward
the goal without ever overestimating.

## Usage

```text
pathfinder [--algo bfs|dijkstra|astar] [--diagonal] [--stats] MAP
```

MAP is a text file, or `-` for stdin: `#` wall, `.` open, `2`-`9` rough
ground, `S` start, `G` goal. `--diagonal` allows eight-way moves (never
cutting the corner of a wall). `--stats` prints the route's length and cost
and how many cells the search looked at.

Exit status: 0 when there is a route, 1 when the goal can't be reached, 2 for
a usage error or a missing or malformed map (the message names the line and
column).

## How it works

- `lib/heap.di` -- `MinHeap`, a binary heap of `Entry` structs. Entries with
  equal priority come out first-in first-out, so a search takes the same route
  on every run.
- `lib/grid.di` -- `Grid` parses the map into flat arrays (cell = `y * width +
  x`) and hands out neighbours and costs. A wall, a ragged line, or a stray
  character raises `MapError` with its position.
- `lib/search.di` -- the three searches. Dijkstra and A* are one function,
  `search_priority`, which takes the heuristic as a `Callable`: `0` for
  Dijkstra, distance-to-goal for A*.

## Checked against a reference

`smoke_test.sh` compares every algorithm's output on two maps, with and
without diagonal moves, against stored results, through the interpreter and a
`diamond build` binary. The searches were also fuzzed against an independent
Python implementation on about four hundred random weighted maps (with and without
walls, diagonals, and unreachable goals): BFS matches the true minimum step
count, and Dijkstra and A* the true minimum cost, on every one.

```bash
bash smoke_test.sh
```
