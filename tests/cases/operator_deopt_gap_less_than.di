class Box
 def initialize(x)
  @x = x
 end
 def <(other) = @x < other.x()
 def x() = @x
end
def lt(a, b) = a < b
i = 0
while i < 5
 lt(1, 2)
 i = i + 1
end
lt(Box.new(3), Box.new(5))
