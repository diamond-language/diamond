class Counter
  def self.set_bounds(arr)
    @@lo, @@hi = arr
  end
  def self.bounds()
    [@@lo, @@hi]
  end

  def initialize()
    @x = 0
    @y = 0
  end
  def set_from(arr)
    @x, @y = arr
  end
  def coords()
    [@x, @y]
  end
end

Counter.set_bounds([1, 10])
c = Counter.new()
c.set_from([5, 7])
[Counter.bounds(), c.coords()]
