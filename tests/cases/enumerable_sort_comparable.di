class Box
  def initialize(value: Int)
    @value = value
  end

  def value() -> Int
    @value
  end

  def <(other: Box) -> Bool
    @value < other.value()
  end

  def >(other: Box) -> Bool
    @value > other.value()
  end

  def to_s() -> String
    "Box(#{@value})"
  end
end

def main()
  boxes = [Box.new(3), Box.new(1), Box.new(2)]
  sorted = boxes.sort()
  index = 0
  while index < sorted.length()
    puts(sorted[index].to_s())
    index = index + 1
  end

  puts(boxes.min().to_s())
  puts(boxes.max().to_s())
end

main()
