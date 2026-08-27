def combine(&block: Callable[2])
  if block_given?()
    yield(2, 3)
  else
    "absent"
  end
end

def combine_rest(*values, &block: Callable[2])
  if block_given?()
    yield(values[0], values[1])
  else
    values
  end
end

class TypedBlockTarget
  def combine(value, &block: Callable[1])
    if block_given?()
      yield(value)
    else
      value
    end
  end
end

puts(combine())
puts(combine() do |left, right|
  left + right
end)

puts(combine_rest(4, 5))
puts(combine_rest(4, 5) do |left, right|
  left * right
end)

target = TypedBlockTarget.new()
puts(target.combine(6))
puts(target.combine(6) do |value|
  value + 1
end)
