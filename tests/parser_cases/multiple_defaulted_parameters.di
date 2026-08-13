def values(a = 20, b = a + 2)
  [a, b]
end

def endless_values(a = 20, b = 99) = [a, b]

class Box
  def initialize(x = 1, y = x + 1, z = 3)
    @x = x
    @y = y
    @z = z
  end

  def sum() = @x + @y + @z
end

[values(), values(40), values(40, 2), endless_values(1, 2), Box.new().sum(), Box.new(10, 20, 30).sum()]
