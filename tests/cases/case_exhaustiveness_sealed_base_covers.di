sealed class Shape
end
class Circle < Shape
end
class Square < Shape
end
class Rounded < Circle
end

def describe(shape: Shape)
  case shape
  when Shape
    "a shape"
  end
end

# A subclass of a covered class is covered too.
def kind(shape: Circle | Square)
  case shape
  when Circle
    "circle"
  when Shape
    "other shape"
  end
end

puts(describe(Circle.new()))
puts(describe(Rounded.new()))
puts(kind(Square.new()))
