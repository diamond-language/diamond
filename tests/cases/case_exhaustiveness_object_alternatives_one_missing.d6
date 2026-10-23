# Comma-joined alternatives cover only the classes they name: Square is
# still missing.
class Circle
end
class Square
end
class Triangle
end
def kind(shape: Circle | Square | Triangle)
  case shape
  when Circle{}, Triangle{}
    "some shape"
  end
end
puts(kind(Circle.new()))
