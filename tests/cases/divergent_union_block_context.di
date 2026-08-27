class CompatibleLeft
  def apply(value: Int, &block: Callable[[Int], Int]) -> Int
    yield(value)
  end
end

class CompatibleRight
  def apply(value: Int, &block: Callable[[Int], Int]) -> Int
    yield(value)
  end
end

condition = ARGV.length() == 0
compatible = if condition
  CompatibleLeft.new()
else
  CompatibleRight.new()
end

puts(compatible.apply(20) do |value|
  value + 1
end)

class IncompatibleLeft
  def apply(value: Int, &block: Callable[[Int], Int]) -> Int
    yield(value)
  end
end

class IncompatibleRight
  def apply(value: String, &block: Callable[[String], String]) -> String
    yield(value)
  end
end

incompatible = if condition
  IncompatibleLeft.new()
else
  IncompatibleRight.new()
end

if !condition
  puts(incompatible.apply(30) do |value|
    value + 1
  end)
end
