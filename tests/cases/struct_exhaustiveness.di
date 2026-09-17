struct Circle(r: Int)
end
struct Square(s: Int)
end
def area(shape: Circle | Square)
  case shape
  when Circle
    shape.r() * shape.r()
  when Square
    shape.s() * shape.s()
  end
end
puts(area(Circle.new(3)))
puts(area(Square.new(4)))
