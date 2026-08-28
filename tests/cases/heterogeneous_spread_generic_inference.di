def second[T, U](first: T, second: U) -> U = second
def middle[T, U](prefix: Int, first: T, second: U, suffix: Int) -> U = second
def apply_second[T, U](first: T, second: U,
                       &block: Callable[[U], Int]) -> Int
  yield(second)
end
def prefix[A, B, C](first: A, middle: B, last: C) -> A = first
def suffix[A, B, C](first: A, middle: B, last: C) -> C = last
def apply_suffix[A, B, C](first: A, middle: B, last: C,
                          &block: Callable[[C], Int]) -> Int
  yield(last)
end

class SpreadGenericBox
  def second[T, U](first: T, second: U) -> U = second
  def self.second[T, U](first: T, second: U) -> U = second
end

class SpreadGenericConsumer
  def initialize[T, U](first: T, second: U,
                       &block: Callable[[U], Int])
    @result = yield(second)
  end
  def result() -> Int = @result
end

box = SpreadGenericBox.new()
puts(second(*["ignored", 40]) + 2)
puts(middle(0, *["ignored", 40], 0) + 2)
puts(SpreadGenericBox.second(*["ignored", 40]) + 2)
puts(box.second(*["ignored", 40]) + 2)
puts(second(*[["ignored"], [40]])[0] + 2)
puts(prefix(40, *["middle"], true) + 2)
puts(suffix(true, *["middle"], 40) + 2)
puts(suffix(true, *["middle"], [40])[0] + 2)
dynamic_middle = ["middle"]
puts(suffix(true, *dynamic_middle, 40) + 2)
puts(apply_second(*["ignored", 40]) do |value|
  value + 2
end)
consumer = SpreadGenericConsumer.new(*["ignored", 40]) do |value|
  value + 2
end
puts(consumer.result())
puts(apply_suffix(true, *["middle"], 40) do |value|
  value + 2
end)
