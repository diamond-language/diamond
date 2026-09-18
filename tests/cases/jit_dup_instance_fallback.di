class Widget
  def initialize(n)
    @n = n
  end
end

def dup_it(x) = x.dup()

w = Widget.new(42)
d = dup_it(w)
puts(d.is_a?(Widget))
