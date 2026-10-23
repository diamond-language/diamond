class Base
  def initialize(x)
    @x = x
  end
  def x()
    @x
  end
end
class Sub < Base
  def initialize(x, y)
    super(x)
    @y = y
  end
  def total()
    self.x() + @y
  end
end
Sub.new(3, 4).total()
