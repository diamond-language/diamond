class Widget
  def initialize(n)
    @n = n
  end
  def bump(other)
    same = self == other
    @n = 99
  end
end
w = Widget.new(1)
w2 = Widget.new(1)
w.freeze()
index = 0
caught = false
while index < 20
  begin
    w.bump(w2)
  rescue error: FrozenError
    caught = true
  end
  index = index + 1
end
puts(caught)
