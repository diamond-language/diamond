def identity[T](value: T) -> T = value
def keyword_middle[A, B, C](first: A, middle: B, last: C) -> B = middle
def keyword_callbacks(first: Callable[[Int], Int],
                      second: Callable[[String], String], label: String) -> Int
  second(label)
  first(42)
end
def keyword_block[A, B, C](first: A, middle: B, last: C,
                           &block: Callable[[B], Int]) -> Int
  yield(middle)
end

class KeywordSpreadBox
  def identity[T](value: T) -> T = value
  def middle[A, B, C](first: A, middle: B, last: C) -> B = middle
  def self.middle[A, B, C](first: A, middle: B, last: C) -> B = middle
end

class KeywordSpreadConsumer
  def initialize[A, B, C](first: A, middle: B, last: C,
                          &block: Callable[[B], Int])
    @result = yield(middle)
  end
  def result() -> Int = @result
end

box = KeywordSpreadBox.new()
puts(keyword_middle(*[true, 40], last: "done") + 2)
puts(KeywordSpreadBox.middle(*[true, 40], last: "done") + 2)
puts(box.middle(*[true, 40], last: "done") + 2)
puts(keyword_callbacks(*[identity, box.identity], label: "diamond"))
puts(keyword_block(*[true, 40], last: "done") do |value|
  value + 2
end)
consumer = KeywordSpreadConsumer.new(*[true, 40], last: "done") do |value|
  value + 2
end
puts(consumer.result())
