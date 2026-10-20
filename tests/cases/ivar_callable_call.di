class Box
  def initialize(cb)
    @cb = cb
  end
  def run(x) = @cb(x)
end
def double(x) = x * 2
Box.new(double).run(21)
