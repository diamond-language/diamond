def worker(f) = f()
def make_capturer(x)
  def inner() = x
  inner
end
Thread.new(worker, make_capturer(5))
