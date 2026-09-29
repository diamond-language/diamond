# A grid is an Array of rows, each an Array[Bool] -- genuinely
# two-dimensional, unlike a flat Array indexed by row * width + col.
# Off the edge of the grid always reads as dead: no wraparound, so a
# pattern that reaches the boundary behaves differently than it would on
# an infinite plane, but stays fully deterministic.

class LifeError < StandardError
end

def life_digits?(text: String) -> Bool
  return false if text.empty?()
  index = 0
  while index < text.length()
    code = text[index].ord()
    return false if code < 48 || code > 57
    index += 1
  end
  true
end

def grid_parse(text: String) -> Array
  lines = text.split("\n").reject() do |line| line.empty?() end
  if lines.empty?()
    raise LifeError.new("pattern is empty")
  end
  width = lines[0].length()
  lines.map() do |line|
    if line.length() != width
      raise LifeError.new("every row must be the same width (#{width})")
    end
    row = []
    index = 0
    while index < line.length()
      char = line[index]
      unless char == "." || char == "#"
        raise LifeError.new("unexpected '#{char}': use '.' for dead, '#' for alive")
      end
      row.push(char == "#")
      index += 1
    end
    row
  end
end

def grid_alive?(grid: Array, row: Int, col: Int) -> Bool
  return false if row < 0 || row >= grid.length()
  return false if col < 0 || col >= grid[row].length()
  grid[row][col]
end

def grid_neighbors(grid: Array, row: Int, col: Int) -> Int
  count = 0
  delta_row = -1
  while delta_row <= 1
    delta_col = -1
    while delta_col <= 1
      unless delta_row == 0 && delta_col == 0
        count += grid_alive?(grid, row + delta_row, col + delta_col) ? 1 : 0
      end
      delta_col += 1
    end
    delta_row += 1
  end
  count
end

# Conway's own rule: a live cell survives with 2 or 3 live neighbors, a
# dead cell is born with exactly 3 -- every other combination dies or
# stays dead.
def grid_step(grid: Array) -> Array
  next_grid = []
  grid.each_with_index() do |row, row_index|
    next_row = []
    row.each_with_index() do |alive, col_index|
      neighbors = grid_neighbors(grid, row_index, col_index)
      next_row.push(alive ? (neighbors == 2 || neighbors == 3) : neighbors == 3)
    end
    next_grid.push(next_row)
  end
  next_grid
end

def grid_render(grid: Array) -> String
  grid.map() do |row|
    row.map() do |alive| alive ? "#" : "." end.join("")
  end.join("\n")
end
