class Base
 def initialize(x)
  @x = x
 end
 def +(other) = Base.new(@x + other.x())
 def x() = @x
end
class Child < Base
 def +(other)
  result = super(other)
  Base.new(result.x() * 10)
 end
end
(Child.new(1) + Child.new(2)).x()
