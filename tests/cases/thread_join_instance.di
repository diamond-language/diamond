class Box
  def initialize(v)
    @v = v
  end
  def get()
    @v
  end
end
def worker()
  Box.new(99)
end
t = Thread.new(worker)
result = t.join()
result.get()
