class Point
 def initialize(x)
  @x = x
 end
 def ==(other)
  other is Point && @x == other.x()
 end
 def x() = @x
end
[Point.new(1) == Point.new(1), Point.new(1) != Point.new(2), Point.new(1) == "not a point"]
