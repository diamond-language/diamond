class Multiplier
  def initialize(factor: Int)
    @factor = factor
  end

  def apply(value: Int) -> Int = @factor * value
end

puts(Multiplier.new(3).apply(4))
