# Three ways to find a route across a Grid, all returning a Found or nil
# when the goal can't be reached:
#
#   bfs       fewest steps; every cell counts the same, so it can pick a
#             route through rough ground that costs more than another
#   dijkstra  lowest total cost
#   astar     lowest total cost, steered toward the goal by a guess
#             (straight-line distance) that never overestimates, so it
#             finds the same cost as dijkstra while looking at fewer cells
require "./grid"
require "./heap"

struct Found(path: Array[Int], cost: Int, expanded: Int)
end

# "Infinity" for costs: a stand-in larger than any real route's cost, used as
# the initial "not reached yet" value in the search tables.
def unreached() = 1_000_000_000

# The route from `start` to `goal`, by walking `came_from` backwards.
def rebuild_path(came_from: Array[Int], start: Int, goal: Int) -> Array[Int]
  # Start at the goal and follow each cell's recorded predecessor back to the
  # start, then flip the list to read start-to-goal.
  path = [goal]
  cell = goal

  while cell != start
    cell = came_from[cell]
    path.push(cell)
  end
  path.reverse()
end

# What a route costs: every cell entered, counting rough ground at its
# price. The start cell is free.
def route_cost(grid: Grid, path: Array[Int]) -> Int
  path.drop(1).sum() do |cell| grid.cost(cell) end
end

# Breadth-first search. `came_from[c]` is the cell we first reached c from;
# `seen` stops a cell being queued twice. The queue is an Array with a moving
# `head` index instead of removing from the front (which would be slow).
# BFS expands cells in order of step count, so the first time it reaches the
# goal is a fewest-steps route.
def search_bfs(grid: Grid, diagonal: Bool) -> Found | Nil
  came_from = [-1] * grid.cells()
  seen = [false] * grid.cells()
  seen[grid.start()] = true
  queue = [grid.start()]
  head = 0

  while head < queue.length()
    cell = queue[head]
    head += 1

    # Reached the goal: rebuild the route. `head` is how many cells have
    # been taken off the queue, which is the "cells looked at" count.
    if cell == grid.goal()
      path = rebuild_path(came_from, grid.start(), cell)
      return Found.new(path, route_cost(grid, path), head)
    end

    # Queue every unvisited neighbor, remembering how we got there.
    grid.neighbors(cell, diagonal).each() do |next_cell|
      next if seen[next_cell]
      seen[next_cell] = true
      came_from[next_cell] = cell
      queue.push(next_cell)
    end
  end
  nil
end

# Dijkstra's algorithm; with `heuristic` it is A*. `heuristic` takes a
# cell and returns a lower bound on the cost still to go.
def search_priority(grid: Grid, diagonal: Bool, heuristic: Callable) -> Found | Nil
  # best[c] is the cheapest known cost to reach c so far. The heap orders
  # cells by cost-so-far plus the heuristic's estimate of the rest.
  best = [unreached()] * grid.cells()
  came_from = [-1] * grid.cells()
  best[grid.start()] = 0
  heap = MinHeap.new()
  heap.push(heuristic(grid.start()), grid.start())
  expanded = 0

  until heap.empty?()
    entry = heap.pop()
    cell = entry.cell()

    # A cell can be queued more than once (each time a cheaper way to it is
    # found), and the heap cannot remove the older, dearer entry. So when an
    # entry comes out whose priority is worse than the cell's best known, it
    # is stale: skip it.
    next if entry.priority() > best[cell] + heuristic(cell)
    expanded += 1

    # With a priority queue, the first time the goal is taken off is the
    # cheapest route (given a heuristic that never overestimates).
    if cell == grid.goal()
      return Found.new(rebuild_path(came_from, grid.start(), cell), best[cell], expanded)
    end

    # Relax each neighbor: if going through this cell is cheaper than the best
    # known way there, record it and queue the neighbor again.
    grid.neighbors(cell, diagonal).each() do |next_cell|
      cost = best[cell] + grid.cost(next_cell)
      if cost < best[next_cell]
        best[next_cell] = cost
        came_from[next_cell] = cell
        heap.push(cost + heuristic(next_cell), next_cell)
      end
    end
  end
  nil
end

# Dijkstra is the priority search with a heuristic that always says 0 (no
# guess about the remaining cost).
def search_dijkstra(grid: Grid, diagonal: Bool) -> Found | Nil
  def none(cell: Int) -> Int
    0
  end
  search_priority(grid, diagonal, none)
end

# A*: the heuristic is the distance to the goal ignoring walls and rough
# ground. Because every step costs at least 1, this is never more than the
# true cost, which is what keeps the answer optimal. With diagonal moves one
# step covers a column and a row at once, so the distance is the larger of
# the two gaps (Chebyshev); with four-way moves it is their sum (Manhattan).
def search_astar(grid: Grid, diagonal: Bool) -> Found | Nil
  goal_x = grid.x_of(grid.goal())
  goal_y = grid.y_of(grid.goal())
  def distance(cell: Int) -> Int
    dx = (grid.x_of(cell) - goal_x).abs()
    dy = (grid.y_of(cell) - goal_y).abs()
    if diagonal then [dx, dy].max() else dx + dy end
  end
  search_priority(grid, diagonal, distance)
end
