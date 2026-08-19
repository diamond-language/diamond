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

low = Box.new(1)
high = Box.new(10)
"#{Box.new(5).between?(low, high)}, #{Box.new(15).between?(low, high)}, #{Box.new(15).clamp(low, high).size()}, #{Box.new(-5).clamp(low, high).size()}, #{Box.new(5).clamp(low, high).size()}"
