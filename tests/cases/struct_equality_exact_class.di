# A struct's generated == requires exactly the same class: a subclass that
# inherits it is not equal to its parent struct's instances, even when the
# struct's own fields match. A subclass that adds fields writes its own ==.
struct Point(x: Int, y: Int)
end

class Point3D < Point
  def initialize(x: Int, y: Int, z: Int)
    super(x, y)
    @z = z
  end

  def z() -> Int = @z

  def ==(other)
    super(other) && other.z() == @z
  end
end

puts(Point.new(1, 2) == Point.new(1, 2))
puts(Point.new(1, 2) == Point.new(1, 3))
puts(Point.new(1, 2) == Point3D.new(1, 2, 3))
puts(Point3D.new(1, 2, 3) == Point.new(1, 2))
puts(Point3D.new(1, 2, 3) == Point3D.new(1, 2, 3))
puts(Point3D.new(1, 2, 3) == Point3D.new(1, 2, 4))
puts(Point.new(1, 2) == 5)
puts(Point.new(1, 2) != Point3D.new(1, 2, 3))
