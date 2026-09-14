sealed class Shape
end
class Circle < Shape
  def initialize(r: Int)
    @r = r
  end
end
class Shape
  def self.describe() = "a shape"
end
Shape.new()
