def maybe_transform(value, &block)
  if block == nil
    value
  else
    block(value)
  end
end

class OptionalBlockTarget
  def transform(value, &block)
    if block == nil
      value + 1
    else
      block(value)
    end
  end
end

class OptionalBlockProxy
  def initialize(target)
    @target = target
  end

  delegate transform(value, &block), to: @target
end

puts(maybe_transform(4))
puts(maybe_transform(4) do |value|
  value * 3
end)

target = OptionalBlockTarget.new()
puts(target.transform(5))
puts(target.transform(5) do |value|
  value * 4
end)

proxy = OptionalBlockProxy.new(target)
puts(proxy.transform(6))
puts(proxy.transform(6) do |value|
  value * 5
end)
