def make() = Shape.new()
sealed class Shape
end
class Circle < Shape
end
make()
