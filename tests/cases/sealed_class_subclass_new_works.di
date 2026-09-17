sealed class Shape
end
class Circle < Shape
  def initialize(r: Int)
    @r = r
  end
end
puts(Circle.new(5).class())
