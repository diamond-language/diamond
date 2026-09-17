class Circle
  def initialize(r: Int)
    @r = r
  end
end
class Square
  def initialize(s: Int)
    @s = s
  end
end
def area(shape: Circle | Square)
  case shape
  when Circle
    3
  when Square
    4
  end
end
puts(area(Circle.new(1)))
puts(area(Square.new(1)))
