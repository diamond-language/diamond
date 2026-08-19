class Box
  include Comparable
  def initialize(size)
    @size = size
  end
  def <=>(other)
    @size - other.size()
  end
  def size() = @size
end

results = [
  Box.new(1) < Box.new(2),
  Box.new(2) < Box.new(1),
  Box.new(1) <= Box.new(1),
  Box.new(2) > Box.new(1),
  Box.new(1) >= Box.new(2),
  Box.new(3) == Box.new(3),
  Box.new(3) == Box.new(4),
]
results
