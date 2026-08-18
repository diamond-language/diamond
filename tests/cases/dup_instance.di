# dup on an Instance shallow-copies every field into a fresh instance of
# the same class -- this is also the regression case for a real bug found
# during this feature's own testing: a naive `fields[]` copy alone reads
# back as all-nil unless the copy's `shape` is also carried over (see
# vm.c's own dup comment for why).
class Point
  def initialize(x, y)
    @x = x
    @y = y
  end
  def x() = @x
  def y() = @y
  def set_x(value)
    @x = value
  end
end

p1 = Point.new(1, 2)
p2 = p1.dup()
p2.set_x(999)
"#{p2.x()}, #{p2.y()}, #{p1.x()}"
