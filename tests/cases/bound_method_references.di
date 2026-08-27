class Calculator
  def add(first, second)
    first + second
  end

  def collect(head, *rest)
    head + ":" + rest.join(",")
  end

  def identity[T](value: T) -> T
    value
  end

  def method_missing(name, arguments)
    "#{name}:#{arguments.join(",")}"
  end
end

class Prefix
  def initialize(value)
    @value = value
  end

  def apply(value)
    @value + value
  end
end

calculator = Calculator.new()
add = calculator.add
collect = calculator.collect
identity = calculator.identity[Int]
missing = calculator.unknown
join = [1, 2, 3].join
prefix = Prefix.new("first-")
apply = prefix.apply
prefix = Prefix.new("second-")

puts(add(10, 3))
puts(collect("head", "a", "b"))
puts(identity(42))
puts(missing("x", "y"))
puts(join("-"))
puts(apply("value"))
nil
