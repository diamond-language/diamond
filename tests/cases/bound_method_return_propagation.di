def retain[T](callback: Callable[[T], T]) -> Callable[[T], T] = callback

class BoundReturnBox
  def increment(value: Int) -> Int = value + 1
  def wrap[T](value: T) -> Array[T] = [value]
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
