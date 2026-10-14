# A struct may extend another struct: its fields follow the parent's, and the
# generated initialize/==/to_s cover all of them; the parent's methods carry over.
struct Point(x: Int, y: Int)
  def norm1() -> Int = x() + y()
end

struct Point3D(z: Int) < Point
  def total() -> Int = norm1() + z()
end

struct Tagged(label: String) < Point3D
end

a = Point3D.new(1, 2, 3)
b = Point3D.new(x: 1, y: 2, z: 3)
puts(a)
puts(a == b)
puts(a == Point3D.new(1, 2, 4))
puts(a == Point.new(1, 2))
puts(Point.new(1, 2) == a)
puts(a.x())
puts(a.z())
puts(a.total())
puts(a.is_a?(Point))
puts(Tagged.new(7, 8, 9, "t"))
puts(Tagged.new(7, 8, 9, "t").norm1())
begin
  Point3D.new("x", 2, 3)
rescue error: TypeError
  puts("type error")
end
def takes(p: Point) -> Int = p.x()
puts(takes(a))
