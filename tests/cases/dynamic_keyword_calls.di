class Formatter
  def render(left, middle, right = "default")
    [left, middle, right].join(":")
  end

  def pair[T](first: T, second: T)
    [first, second]
  end
end

def subtract(left, right)
  left - right
end

formatter = Formatter.new()
puts(formatter.render(right: "r", left: "l", middle: "m"))
puts(formatter.render(*["a"], middle: "b", right: "c"))
puts(formatter.pair[Int](*[10], second: 20).join(","))
callable = subtract
puts(callable(right: 3, left: 10))
puts(callable(*[20], right: 4))
nil
