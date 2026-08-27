class Configured
  def initialize(value, &block)
    if block_given?()
      @value = yield(value)
    else
      @value = value
    end
  end

  def value()
    @value
  end
end

plain = Configured.new(3)
puts(plain.value())

fixed = Configured.new(4) do |value|
  value * 2
end
puts(fixed.value())

spread = Configured.new(*[5]) do |value|
  value + 10
end
puts(spread.value())
