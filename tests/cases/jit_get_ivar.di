class Counter
  def initialize(start)
    @value = start
  end

  def touch_loop()
    i = 0
    while i < 100000
      v = @value
      i = i + 1
    end
    @value
  end
end

c = Counter.new(3)
puts(c.touch_loop())
