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
  def s() = @s
end
def area(shape: Circle | Square)
  case shape
  when Circle{}
    3.14159 * shape.r() * shape.r()
  when Square{}
    shape.s() * shape.s()
  end
end
puts(area(Circle.new(2)))
puts(area(Square.new(3)))
