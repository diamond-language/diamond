def retain[T](callback: Callable[[T], T]) -> Callable[[T], T] = callback

class BoundReturnBox
  def increment(value: Int) -> Int = value + 1
  def wrap[T](value: T) -> Array[T] = [value]
  def sum(head: Int, *rest) -> Int = head + rest[0]
  def offset(value: Int, amount: Int = 0) -> Int = value + amount
  def label(prefix: String = "default", *rest) -> String = prefix + ":" + rest.join(",")
  def self.wrap[T](value: T) -> Array[T] = [value]
end

box = BoundReturnBox.new()
increment = box.increment
puts(increment(40) + 1)

generic_wrap = box.wrap[Int]
puts(generic_wrap(39)[0] + 3)

singleton_wrap = BoundReturnBox.wrap[Int]
puts(singleton_wrap(38)[0] + 4)

puts(retain(box.increment)(40) + 1)

sum = box.sum
puts(sum(40, 2) + 0)

offset = box.offset
puts(offset(value: 39, amount: 3) + 0)
puts(offset(value: 42) + 0)

label = box.label
puts(label())
puts(label("set", "x"))
