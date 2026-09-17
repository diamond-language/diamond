class Point
  def initialize(x: Int, y: Int)
    @x = x
    @y = y
  end
  def bump()
    @x = @x + 1
  end
  def x() = @x
end
p = Point.new(1, 2)
p.freeze()
puts(p.frozen?())
begin
  p.bump()
rescue error: FrozenError
  puts("ivar write blocked")
end
puts(p.x())
