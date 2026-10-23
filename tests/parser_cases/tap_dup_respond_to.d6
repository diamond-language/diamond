seen = []
result = 5.tap() do |x|
  seen.push(x)
end
puts(result)
puts(seen)

a = [1, 2, 3]
b = a.dup()
b.push(4)
puts(a)
puts(b)

class Point
  def initialize(x, y)
    @x = x
    @y = y
  end
  def x() = @x
  def y() = @y
end
p1 = Point.new(1, 2)
p2 = p1.dup()
puts(p2.x())
puts(p2.y())

class Greeter
  def hello() = "hi"
  private def secret() = "shh"
end
g = Greeter.new()
puts(g.respond_to?(:hello))
puts(g.respond_to?(:secret))
puts(g.respond_to?(:frobnicate))
nil
