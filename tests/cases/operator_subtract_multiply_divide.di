class Box
 def initialize(x)
  @x = x
 end
 def -(other) = Box.new(@x - other.x())
 def *(other) = Box.new(@x * other.x())
 def /(other) = Box.new(@x / other.x())
 def x() = @x
end
[(Box.new(10) - Box.new(3)).x(), (Box.new(4) * Box.new(5)).x(), (Box.new(20) / Box.new(4)).x()]
