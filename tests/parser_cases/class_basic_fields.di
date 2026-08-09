class Point
  def initialize(x, y)
    @x = x
    @y = y
  end
  def x()
    @x
  end
  def y()
    @y
  end
  def sum()
    self.x() + self.y()
  end
end
p = Point.new(3, 4)
p.sum()
