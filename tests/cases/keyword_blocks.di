def combine(left, right, &block)
  yield(left, right)
end

puts(combine(right: 3, left: 2) do |left, right|
  left + right
end)

callable = combine
puts(callable(right: 5, left: 4) do |left, right|
  left * right
end)

puts(callable(10, right: 11) do |left, right|
  left + right
end)

class KeywordBlockTarget
  def combine(left, right, &block)
    yield(left, right)
  end
end

target = KeywordBlockTarget.new()
puts(target.combine(right: 7, left: 6) do |left, right|
  right - left
end)

class KeywordBlockConstructor
  def initialize(left, right, &block)
    @value = yield(left, right)
  end

  def value()
    @value
  end
end

configured = KeywordBlockConstructor.new(right: 9, left: 8) do |left, right|
  left + right
end
puts(configured.value())
