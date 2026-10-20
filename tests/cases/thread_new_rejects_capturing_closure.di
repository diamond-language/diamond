x = 5
def make_capturer(x)
  def inner() = x
  inner
end
Thread.new(make_capturer(x))
