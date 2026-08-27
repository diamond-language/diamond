class VariadicBlockTarget
  def transform(first, second, third, callback: Callable[1])
    callback(first + second + third)
  end
end

class VariadicBlockProxy
  def initialize(target)
    @target = target
  end

  delegate transform(first, *remaining, &block), to: @target
end

puts(VariadicBlockProxy.new(VariadicBlockTarget.new()).transform(2, 3, 4) do |sum|
  sum * 5
end)
