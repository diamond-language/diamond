def transform(value, &block)
  puts(block_given?())
  if block_given?()
    yield(value, value + 1)
  else
    value
  end
end

def invoke_empty(&block)
  yield
end

def lexical_boundary(&handler)
  def probe()
    block_given?()
  end
  puts(probe())
  yield(2)
end

class YieldTarget
  def transform(value, &block)
    puts(block_given?())
    if block_given?()
      yield(value)
    else
      value + 1
    end
  end
end

puts(transform(4))
puts(transform(4) do |left, right|
  left + right
end)

puts(invoke_empty() do
  "empty"
end)

puts(lexical_boundary() do |value|
  value * 4
end)

target = YieldTarget.new()
puts(target.transform(5))
puts(target.transform(5) do |value|
  value * 3
end)
