# One alternative constrains a reader (r must be 1), so it covers only some
# Circles: Circle is still uncovered even though Square{} is, in the same
# comma-joined clause.
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
  when Circle{r: 1}, Square{}
    "some shape"
  end
end
puts(kind(Circle.new(1)))
