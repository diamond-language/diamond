def calculate(&block: Callable[[Int], Int]) -> Int
  yield(6)
end

def format(&block: Callable[[String], String]) -> String
  yield("diamond")
end

class ContextOps
  def self.calculate(&block: Callable[[Int], Int]) -> Int
    yield(9)
  end
end

class ContextBase
  def transform(&block: Callable[[String], String]) -> String
    yield("instance")
  end
end

class ContextChild < ContextBase
end

puts(calculate() do |value|
  value * 7
end)

puts(format() do |value|
  value + "!"
end)

puts(ContextOps.calculate() do |value|
  value + 1
end)

puts(ContextBase.new().transform() do |value|
  value + "!"
end)

puts(ContextChild.new().transform() do |value|
  value + " inherited"
end)

arguments = []
puts(ContextChild.new().transform(*arguments) do |value|
  value + " spread"
end)
