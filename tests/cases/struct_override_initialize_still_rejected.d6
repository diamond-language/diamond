# Only == and to_s may replace generated members; initialize (and the field
# readers) still collide.
struct Point(x: Int, y: Int)
  def initialize(x: Int, y: Int)
    @x = x
    @y = y
  end
end
puts(Point.new(1, 2))
