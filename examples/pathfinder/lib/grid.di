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

  def initialize(text: String)
    lines = text.split("\n").reject() do |line| line.strip().empty?() end
    raise MapError.new("the map is empty") if lines.empty?()
    @height = lines.length()
    @width = lines[0].length()
    @cost = []
    @start = -1
    @goal = -1
    lines.each_with_index() do |line, y|
      if line.length() != @width
        raise MapError.new("line #{y + 1} is #{line.length()} wide, expected #{@width}")
      end
      line.chars().each_with_index() do |ch, x|
        @cost.push(self.cost_of(ch, x, y))
        @start = y * @width + x if ch == "S"
        @goal = y * @width + x if ch == "G"
      end
    end
    raise MapError.new("no start (S) on the map") if @start < 0
    raise MapError.new("no goal (G) on the map") if @goal < 0
  end

  def cells() = @cost.length()

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
    [[0, -1], [1, 0], [0, 1], [-1, 0], [1, -1], [1, 1], [-1, 1], [-1, -1]].each_with_index() do |step, index|
      next if index >= 4 && !diagonal
      nx = x + step[0]
      ny = y + step[1]
      next if nx < 0 || ny < 0 || nx >= @width || ny >= @height
      next if @cost[ny * @width + nx] == 0
      if index >= 4 && (@cost[y * @width + nx] == 0 || @cost[ny * @width + x] == 0)
        next
      end
      found.push(ny * @width + nx)
    end
    found
  end

  # The map with the cells of `path` drawn as `*`.
  def render(path: Array[Int]) -> String
    on_path = [false] * @cost.length()
    path.each() do |cell| on_path[cell] = true end
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

  def cost_of(ch: String, x: Int, y: Int) -> Int
    return 0 if ch == "#"
    return 1 if ch == "." || ch == "S" || ch == "G"
    if ch.length() == 1 && ch >= "2" && ch <= "9"
      return ch.to_i()
    end
    raise MapError.new("unexpected '#{ch}' at line #{y + 1}, column #{x + 1}")
  end

  def glyph(cell: Int) -> String
    if @cost[cell] == 0 then "#" elsif @cost[cell] == 1 then "." else @cost[cell].to_s() end
  end
end
