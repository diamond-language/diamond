def second[T, U](first: T, second: U) -> U = second
def middle[T, U](prefix: Int, first: T, second: U, suffix: Int) -> U = second
def apply_second[T, U](first: T, second: U,
                       &block: Callable[[U], Int]) -> Int
  yield(second)
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
puts(apply_second(*["ignored", 40]) do |value|
  value + 2
end)
consumer = SpreadGenericConsumer.new(*["ignored", 40]) do |value|
  value + 2
end
puts(consumer.result())
