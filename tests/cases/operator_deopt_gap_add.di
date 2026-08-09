class Box
 def initialize(x)
  @x = x
 end
 def +(other) = Box.new(@x + other.x())
 def x() = @x
end
def add(a, b) = a + b
i = 0
while i < 5
 add(1, 2)
 i = i + 1
end
add(Box.new(10), Box.new(5)).x()
