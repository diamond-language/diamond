class Box
  def initialize(cb)
    @cb = cb
  end
  def run(x)
    @cb(x)
  end
end
def double(x)
  x * 2
end
Box.new(double).run(21)
