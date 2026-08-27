def nested[T](value: T) -> Array[Array[T]] = [[value]]
def optional[T](value: T) -> T | Nil = value
def keep[T](callback: Callable[[T], T]) -> Callable[[T], T] = callback
def plus_one(value: Int) -> Int = value + 1

class ReturnGeneric
  def self.wrap[T](value: T) -> Array[T] = [value]
  def wrap[T](value: T) -> Array[T] = [value]
end

puts(nested(40)[0][0] + 2)
puts(nested[Int](39)[0][0] + 3)
puts(nested(*[38])[0][0] + 4)
puts(ReturnGeneric.wrap(value: 37)[0] + 5)
puts(ReturnGeneric.wrap[Int](36)[0] + 6)

factory = ReturnGeneric.new()
puts(factory.wrap(35)[0] + 7)
puts(factory.wrap[Int](34)[0] + 8)

maybe = optional(33)
if maybe is Int
  puts(maybe + 9)
end

puts(keep[Int](plus_one)(40) + 1)
