# A map read from text. One character per cell:
#
#   #        wall
#   .        open ground, cost 1
#   2 .. 9   rough ground, that cost to enter
#   S, G     start and goal (open ground)
#
# Cells are numbered row by row, so cell = y * width + x, and the search
# code keeps its per-cell tables in flat arrays instead of hashes.
class MapError < StandardError
end

class Grid
  attr_reader width: Int
  attr_reader height: Int
  attr_reader start: Int
  attr_reader goal: Int

  # Parses the map text, validating as it goes (every error says where).
  def initialize(text: String)
    # Ignore blank lines (e.g. a trailing newline); the first real line sets
    # the width every other line must match.
    lines = text.split("\n").reject() do |line| line.strip().empty?() end
    raise MapError.new("the map is empty") if lines.empty?()
    @height = lines.length()
    @width = lines[0].length()

    # @cost is flat: one entry per cell, row by row. -1 means "not found yet"
    # for the start and goal positions.
    @cost = []
    @start = -1
    @goal = -1

    lines.each_with_index() do |line, y|
      if line.length() != @width
        raise MapError.new("line #{y + 1} is #{line.length()} wide, expected #{@width}")
      end

      # For each character: record the cell's cost (cost_of rejects unknown
      # characters), and note where S and G are as flat cell numbers.
      line.chars().each_with_index() do |ch, x|
        @cost.push(self.cost_of(ch, x, y))
        @start = y * @width + x if ch == "S"
        @goal = y * @width + x if ch == "G"
      end
    end

    # A map needs both endpoints.
    raise MapError.new("no start (S) on the map") if @start < 0
    raise MapError.new("no goal (G) on the map") if @goal < 0
  end

  # Total number of cells, for sizing the search's per-cell tables.
  def cells() = @cost.length()

  # Convert a flat cell number back to coordinates.

  def x_of(cell: Int) -> Int = cell % @width

  def y_of(cell: Int) -> Int = cell / @width

  # What it costs to step onto a cell; 0 means a wall.
  def cost(cell: Int) -> Int = @cost[cell]

  # The cells you can step to from `cell`: four ways, or eight with
  # `diagonal` (never through the corner of a wall).
  def neighbors(cell: Int, diagonal: Bool) -> Array[Int]
    x = self.x_of(cell)
    y = self.y_of(cell)
    found = []

    # The eight (dx, dy) steps. The first four (up, right, down, left) are
    # always allowed; entries 4-7 are the diagonals, so `index >= 4` below
    # means "this is a diagonal step".
    [[0, -1], [1, 0], [0, 1], [-1, 0], [1, -1], [1, 1], [-1, 1], [-1, -1]].each_with_index() do |step, index|
      next if index >= 4 && !diagonal
      nx = x + step[0]
      ny = y + step[1]

      # Skip anything off the map, or a wall.
      next if nx < 0 || ny < 0 || nx >= @width || ny >= @height
      next if @cost[ny * @width + nx] == 0

      # A diagonal move may not squeeze between two walls touching at a
      # corner: both orthogonal cells it passes (same row / same column as
      # the destination) must be open.
      if index >= 4 && (@cost[y * @width + nx] == 0 || @cost[ny * @width + x] == 0)
        next
      end
      found.push(ny * @width + nx)
    end
    found
  end

  # The map with the cells of `path` drawn as `*`.
  def render(path: Array[Int]) -> String
    # A flat lookup table: is this cell on the route?
    on_path = [false] * @cost.length()
    path.each() do |cell| on_path[cell] = true end

    # Draw row by row. S and G are always shown (never overwritten by the
    # route's `*`), then route cells, then the plain terrain.
    rows = []
    @height.times() do |y|
      row = ""
      @width.times() do |x|
        cell = y * @width + x
        row += if cell == @start
          "S"
        elsif cell == @goal
          "G"
        elsif on_path[cell]
          "*"
        else
          self.glyph(cell)
        end
      end
      rows.push(row)
    end
    rows.join("\n")
  end

  private

  # The cost of the cell a character stands for (0 = wall). Anything not in
  # the legend is an error that names its line and column.
  def cost_of(ch: String, x: Int, y: Int) -> Int
    return 0 if ch == "#"
    return 1 if ch == "." || ch == "S" || ch == "G"
    if ch.length() == 1 && ch >= "2" && ch <= "9"
      return ch.to_i()
    end
    raise MapError.new("unexpected '#{ch}' at line #{y + 1}, column #{x + 1}")
  end

  # The character to draw for an off-route cell (the inverse of cost_of).
  def glyph(cell: Int) -> String
    if @cost[cell] == 0 then "#" elsif @cost[cell] == 1 then "." else @cost[cell].to_s() end
  end
end
