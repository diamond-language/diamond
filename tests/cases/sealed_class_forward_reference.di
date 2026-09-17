def make()
  Shape.new()
end
sealed class Shape
end
class Circle < Shape
end
make()
