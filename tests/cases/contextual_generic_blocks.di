def generic_apply[T](value: T, &block: Callable[[T], T]) -> T
  yield(value)
end

def generic_array_apply[T](values: Array[T], &block: Callable[[T], T]) -> T
  yield(values[0])
end

def generic_hash_apply[K, V](values: Hash[K, V], &block: Callable[[V], V]) -> V
  yield(values["answer"])
end

def generic_nested_apply[T](values: Array[Array[T]], &block: Callable[[T], T]) -> T
  yield(values[0][0])
end

puts(generic_apply[Int](6) do |value|
  value * 7
end)

puts(generic_apply(7) do |value|
  value * 6
end)

puts(generic_array_apply([9]) do |value|
  value + 3
end)

puts(generic_hash_apply({"answer": 40}) do |value|
  value + 2
end)

puts(generic_nested_apply([[5], [6]]) do |value|
  value * 2
end)

puts(generic_apply[Int](*[8]) do |value|
  value * 8
end)

puts(generic_apply(*[9]) do |value|
  value * 7
end)

class GenericBlockOps
  def self.apply[T](value: T, &block: Callable[[T], T]) -> T
    yield(value)
  end

  def apply[T](value: T, &block: Callable[[T], T]) -> T
    yield(value)
  end
end

class GenericBlockBox
  def initialize[T](value: T, &block: Callable[[T], T])
    @value = yield(value)
  end

  def value()
    @value
  end
end

puts(GenericBlockOps.apply[String]("singleton") do |value|
  value + " generic"
end)

puts(GenericBlockOps.apply("implicit singleton") do |value|
  value + " generic"
end)

puts(GenericBlockOps.apply(*[20]) do |value|
  value + 1
end)

puts(GenericBlockOps.new().apply[Int](10) do |value|
  value + 5
end)

puts(GenericBlockOps.new().apply(20) do |value|
  value + 2
end)

puts(GenericBlockOps.new().apply(*[30]) do |value|
  value + 3
end)

box = GenericBlockBox.new(30) do |value|
  value + 12
end
puts(box.value())

spread_box = GenericBlockBox.new(*[40]) do |value|
  value + 2
end
puts(spread_box.value())
