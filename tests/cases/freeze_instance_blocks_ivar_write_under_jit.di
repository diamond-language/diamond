class Counter
  def initialize(n: Int)
    @n = n
  end
  def bump()
    @n = @n + 1
  end
end
c = Counter.new(0)
c.freeze()
index = 0
caught = false
while index < 20
  begin
    c.bump()
  rescue error: FrozenError
    caught = true
  end
  index = index + 1
end
puts(caught)
