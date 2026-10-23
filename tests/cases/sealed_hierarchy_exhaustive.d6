sealed class Shape
end
class Circle < Shape
  def initialize(r: Int)
    @r = r
  end
end
class Square < Shape
  def initialize(s: Int)
    @s = s
  end
end
def area(shape: Shape)
  case shape
  when Circle
    3
  when Square
    4
  end
end
puts(area(Circle.new(1)))
puts(area(Square.new(1)))
