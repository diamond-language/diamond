# A guard can reject the match at runtime, so comma-joined alternatives
# under an `if` cover nothing.
class Circle
  def initialize(r: Int)
    @r = r
  end
  def r() = @r
end
class Square
  def initialize(s: Int)
    @s = s
  end
end
def kind(shape: Circle | Square)
  case shape
  when Circle{}, Square{} if false
    "some shape"
  end
end
puts(kind(Circle.new(1)))
