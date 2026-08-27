def inspect_tail(*values, &block)
  puts(values)
  puts(block == nil)
end

def ordinary_callable(value)
  value
end

def summarize_tail(*values, &block)
  [values.length(), block == nil]
end

inspect_tail(1, 2, 3)
inspect_tail(4, 5) do
  9
end
puts(summarize_tail(6, ordinary_callable))

class OptionalVariadicTarget
  def inspect(prefix, *values, &block)
    puts(prefix)
    puts(values)
    puts(block == nil)
  end
end

class OptionalVariadicProxy
  def initialize(target)
    @target = target
  end

  delegate inspect(prefix, *values, &block), to: @target
end

proxy = OptionalVariadicProxy.new(OptionalVariadicTarget.new())
proxy.inspect("plain", 6, 7)
proxy.inspect("blocked", 8) do
  10
end
