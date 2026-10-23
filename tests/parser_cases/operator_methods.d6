class Vector
 def initialize(x, y)
  @x = x
  @y = y
 end
 def +(other)
  Vector.new(@x + other.x(), @y + other.y())
 end
 def ==(other)
  @x == other.x() && @y == other.y()
 end
 def <(other)
  @x < other.x()
 end
 def x() = @x
 def y() = @y
end
a = Vector.new(1, 2)
b = Vector.new(3, 4)
sum = a + b
puts(sum.x())
puts(sum.y())
puts(a == Vector.new(1, 2))
puts(a < b)
nil
