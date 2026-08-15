class Box
  def initialize(cb)
    @cb = cb
  end
  def run()
    @cb()
  end
end
def needs_arg(x)
  x
end
Box.new(needs_arg).run()
