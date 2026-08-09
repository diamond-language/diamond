class Vector
 def initialize(x, y)
  @x = x
  @y = y
 end
 def +(other)
  Vector.new(@x + other.x(), @y + other.y())
 end
 def x() = @x
 def y() = @y
end
v = Vector.new(1, 2) + Vector.new(3, 4)
[v.x(), v.y()]
