sealed class Shape
end
class Circle < Shape
  def initialize(r: Int)
    @r = r
  end
  def r() = @r
end
class Square < Shape
  def initialize(s: Int)
    @s = s
  end
  def s() = @s
end
def area(shape: Shape)
  case shape
  when Circle{}
    3.14159 * shape.r() * shape.r()
  when Square{}
    shape.s() * shape.s()
  end
end
puts(area(Circle.new(2)))
puts(area(Square.new(3)))
