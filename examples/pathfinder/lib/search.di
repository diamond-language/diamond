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

def unreached() = 1_000_000_000

# The route from `start` to `goal`, by walking `came_from` backwards.
def rebuild_path(came_from: Array[Int], start: Int, goal: Int) -> Array[Int]
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

def search_bfs(grid: Grid, diagonal: Bool) -> Found | Nil
  came_from = [-1] * grid.cells()
  seen = [false] * grid.cells()
  seen[grid.start()] = true
  queue = [grid.start()]
  head = 0
  while head < queue.length()
    cell = queue[head]
    head += 1
    if cell == grid.goal()
      path = rebuild_path(came_from, grid.start(), cell)
      return Found.new(path, route_cost(grid, path), head)
    end
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
  best = [unreached()] * grid.cells()
  came_from = [-1] * grid.cells()
  best[grid.start()] = 0
  heap = MinHeap.new()
  heap.push(heuristic(grid.start()), grid.start())
  expanded = 0
  until heap.empty?()
    entry = heap.pop()
    cell = entry.cell()
    # A cell can be queued more than once; only its cheapest entry counts.
    next if entry.priority() > best[cell] + heuristic(cell)
    expanded += 1
    if cell == grid.goal()
      return Found.new(rebuild_path(came_from, grid.start(), cell), best[cell], expanded)
    end
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

def search_dijkstra(grid: Grid, diagonal: Bool) -> Found | Nil
  def none(cell: Int) -> Int
    0
  end
  search_priority(grid, diagonal, none)
end

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
