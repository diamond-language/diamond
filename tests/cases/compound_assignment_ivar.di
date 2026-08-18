class Counter
  def initialize()
    @n = 0
  end
  def bump()
    @n += 1
    @n
  end
end
c = Counter.new()
c.bump()
c.bump()
c.bump()
