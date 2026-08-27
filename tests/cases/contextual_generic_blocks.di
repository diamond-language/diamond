def generic_apply[T](value: T, &block: Callable[[T], T]) -> T
  yield(value)
end

puts(generic_apply[Int](6) do |value|
  value * 7
end)

puts(generic_apply[Int](*[8]) do |value|
  value * 8
end)

class GenericBlockOps
  def self.apply[T](value: T, &block: Callable[[T], T]) -> T
    yield(value)
  end

  def apply[T](value: T, &block: Callable[[T], T]) -> T
    yield(value)
  end
end

puts(GenericBlockOps.apply[String]("singleton") do |value|
  value + " generic"
end)

puts(GenericBlockOps.new().apply[Int](10) do |value|
  value + 5
end)
