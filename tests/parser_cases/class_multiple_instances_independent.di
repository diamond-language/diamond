class Box
  def initialize(v)
    @v = v
  end
  def get()
    @v
  end
end
a = Box.new(10)
b = Box.new(20)
a.get() + b.get()
