class Widget
  def initialize(n)
    @n = n
  end
  def n() = @n
  def set(v)
    @n = v
  end
end

def dup_it(x) = x.dup()
def freeze_it(x) = x.freeze()
def frozen_it(x) = x.frozen?()

w = Widget.new(42)
d = dup_it(w)
d.set(100)
puts(d.n())
puts(w.n())

freeze_it(w)
puts(frozen_it(w))
w2 = Widget.new(1)
puts(frozen_it(w2))
