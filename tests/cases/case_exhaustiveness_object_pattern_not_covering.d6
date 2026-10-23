# A pattern that constrains a reader (here r must be 1) matches only some
# Circles, so it doesn't cover Circle. (A binding-only Circle{r: r} does.)
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
def area(shape: Circle | Square)
  case shape
  when Circle{r: 1}
    1
  when Square
    4
  end
end
puts(area(Circle.new(1)))
