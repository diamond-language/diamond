# JIT Phase 16: declared-return chaining through an ivar-loaded receiver.
class Value
  def initialize(n)
    @n = n
  end
  def number() -> Int = @n
end

class Factory
  def make(value: Value) -> Value = value
end

class Runner
  def initialize(factory: Factory, value: Value)
    @factory = factory
    @value = value
  end
  def run(iterations) -> Int
    result = @factory.make(@value)
    total = 0
    i = 0
    while i < iterations
      total = total + result.number()
      i = i + 1
    end
    total
  end
end

puts(Runner.new(Factory.new(), Value.new(7)).run(50000000))
