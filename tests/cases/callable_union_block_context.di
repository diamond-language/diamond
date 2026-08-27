def block_left(value: Int, &block: Callable[[Int], Int]) -> Int = yield(value)
def block_right(value: Int, &block: Callable[[Int], Int]) -> Int = yield(value)
def block_different(value: Int, &block: Callable[[String], String]) -> Int = 0

class ContextAnimal
  def score() -> Int = 40
end

class ContextDog < ContextAnimal
end

def with_dog(&block: Callable[[ContextDog], Int]) -> Int = yield(ContextDog.new())
def with_animal(&block: Callable[[ContextAnimal], Int]) -> Int = yield(ContextAnimal.new())

agreed = if ARGV.length() == 0
  block_left
else
  block_right
end
puts(agreed(40) do |value|
  value + 2
end)

divergent = if ARGV.length() == 0
  block_left
else
  block_different
end
puts(divergent(39) do |value|
  value + 3
  42
end)

variant = if ARGV.length() == 0
  with_dog
else
  with_animal
end
puts(variant() do |value|
  value.score() + 2
end)
