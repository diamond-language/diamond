def block_left(value: Int, &block: Callable[[Int], Int]) -> Int = yield(value)
def block_right(value: Int, &block: Callable[[Int], Int]) -> Int = yield(value)
def block_different(value: Int, &block: Callable[[String], String]) -> Int = 0

class ContextAnimal
  def score() -> Int = 40
end

class ContextDog < ContextAnimal
end

class ContextRed
  def score() -> Int = 40
end

class ContextBlue
  def score() -> Int = 40
end

def with_dog(&block: Callable[[ContextDog], Int]) -> Int = yield(ContextDog.new())
def with_animal(&block: Callable[[ContextAnimal], Int]) -> Int = yield(ContextAnimal.new())
def dog_object_contract(&block: Callable[[ContextDog], ContextDog]) -> Int
  yield(ContextDog.new())
  40
end
def animal_object_contract(&block: Callable[[ContextAnimal], ContextAnimal]) -> Int
  yield(ContextAnimal.new())
  40
end
def dog_array_contract(&block: Callable[[], Array[ContextDog]]) -> Int
  yield()
  40
end
def animal_array_contract(&block: Callable[[], Array[ContextAnimal]]) -> Int
  yield()
  40
end
def with_red(&block: Callable[[ContextRed], Int]) -> Int
  yield(ContextRed.new())
end
def with_blue(&block: Callable[[ContextBlue], Int]) -> Int
  yield(ContextBlue.new())
end

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

synthesized = if ARGV.length() == 0
  dog_object_contract
else
  animal_object_contract
end
puts(synthesized() do |value|
  value.score() + 2
  ContextDog.new()
end + 2)

synthesized_return = if ARGV.length() == 0
  dog_array_contract
else
  animal_array_contract
end
puts(synthesized_return() do
  []
end + 2)

constructed_union = if ARGV.length() == 0
  with_red
else
  with_blue
end
puts(constructed_union() do |value|
  value.score() + 2
end)
