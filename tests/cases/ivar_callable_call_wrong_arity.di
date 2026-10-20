class Box
  def initialize(cb)
    @cb = cb
  end
  def run() = @cb()
end
def needs_arg(x) = x
Box.new(needs_arg).run()
