def apply_int(callback: Callable[[Int], Int]) -> Int
  callback(41)
end

def identity[T](value: T) -> T = value

class ContextualReferenceBox
  def identity[T](value: T) -> T = value
  def accept(callback: Callable[[Int], Int]) -> Int = callback(40)
  def self.identity[T](value: T) -> T = value
  def self.accept(callback: Callable[[Int], Int]) -> Int = callback(39)
end

class ContextualReferenceConsumer
  def initialize(callback: Callable[[Int], Int])
    @callback = callback
  end
  def run() -> Int = @callback(38)
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
