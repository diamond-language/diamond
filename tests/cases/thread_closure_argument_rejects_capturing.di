def worker(f)
  f()
end
def make_capturer(x)
  def inner()
    x
  end
  inner
end
Thread.new(worker, make_capturer(5))
