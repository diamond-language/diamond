class Point
  attr_accessor px, py
  def initialize(x, y)
    @px = x
    @py = y
  end
  def ==(other)
    self.px() == other.px() && self.py() == other.py()
  end
end

def compare(a, b) = a == b

def run()
  total = 0
  index = 0
  while index < 10
    a = Point.new(1, 2)
    b = Point.new(1, 2)
    total = total + (if compare(a, b) then 1 else 0 end)
    index = index + 1
  end
  total
end
run()
