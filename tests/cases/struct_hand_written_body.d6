struct Point(x: Int, y: Int)
  def distance_squared_to(other)
    dx = @x - other.x()
    dy = @y - other.y()
    dx * dx + dy * dy
  end
end

p1 = Point.new(0, 0)
p2 = Point.new(3, 4)
puts(p1.distance_squared_to(p2))
