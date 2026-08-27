def apply_int(callback: Callable[[Int], Int]) -> Int
  callback(41)
end

def identity[T](value: T) -> T = value
def apply_all(callbacks: Array[Callable[[Int], Int]]) -> Int
  callbacks[0](42)
end
def apply_map(callbacks: Hash[String, Callable[[Int], Int]]) -> Int
  callbacks["chosen"](42)
end
def apply_nested_map(callbacks: Array[Hash[String, Callable[[Int], Int]]]) -> Int
  callbacks[0]["chosen"](42)
end
def apply_spread(first: Callable[[Int], Int], second: Callable[[Int], Int]) -> Int
  first(20) + second(22)
end

class ContextualReferenceBox
  def identity[T](value: T) -> T = value
  def accept(callback: Callable[[Int], Int]) -> Int = callback(40)
  def self.identity[T](value: T) -> T = value
  def self.accept(callback: Callable[[Int], Int]) -> Int = callback(39)
  def accept_spread(first: Callable[[Int], Int], second: Callable[[Int], Int]) -> Int
    first(20) + second(22)
  end
  def self.accept_spread(first: Callable[[Int], Int], second: Callable[[Int], Int]) -> Int
    first(20) + second(22)
  end
end

class ContextualReferenceConsumer
  def initialize(callback: Callable[[Int], Int])
    @callback = callback
  end
  def run() -> Int = @callback(38)
end

class ContextualSpreadConsumer
  def initialize(first: Callable[[Int], Int], second: Callable[[Int], Int])
    @first = first
    @second = second
  end
  def run() -> Int = @first(20) + @second(22)
end

box = ContextualReferenceBox.new()
puts(apply_int(identity) + 1)
puts(apply_int(ContextualReferenceBox.identity) + 1)
puts(apply_int(box.identity) + 1)
puts(ContextualReferenceBox.accept(ContextualReferenceBox.identity) + 2)
puts(box.accept(box.identity) + 2)
puts(ContextualReferenceBox.accept(
  callback: ContextualReferenceBox.identity
) + 3)
puts(box.accept(callback: box.identity) + 3)
consumer = ContextualReferenceConsumer.new(callback: box.identity)
puts(consumer.run() + 4)
puts(apply_all([ContextualReferenceBox.identity]))
puts(apply_all([box.identity]))
puts(apply_map({"chosen": ContextualReferenceBox.identity}))
puts(apply_map({"chosen": box.identity}))
puts(apply_nested_map([{"chosen": ContextualReferenceBox.identity}]))
puts(apply_nested_map([{"chosen": box.identity}]))
puts(apply_spread(*[ContextualReferenceBox.identity, box.identity]))
puts(ContextualReferenceBox.accept_spread(
  *[ContextualReferenceBox.identity, box.identity]
))
puts(box.accept_spread(*[ContextualReferenceBox.identity, box.identity]))
spread_consumer = ContextualSpreadConsumer.new(
  *[ContextualReferenceBox.identity, box.identity]
)
puts(spread_consumer.run())
