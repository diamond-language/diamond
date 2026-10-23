class AgreeLeft
  def wrap[T](value: T) -> Array[T] = [value]
end

class AgreeRight
  def wrap[T](value: T) -> Array[T] = [value]
end

class DivergeLeft
  def wrap[T](value: T) -> Array[T] = [value]
end

class DivergeRight
  def wrap[T](value: T) -> Array[String] = ["dynamic"]
end

agreed = if ARGV.length() == 0
  AgreeLeft.new()
else
  AgreeRight.new()
end
puts(agreed.wrap(40)[0] + 2)

divergent = if ARGV.length() == 0
  DivergeLeft.new()
else
  DivergeRight.new()
end
puts(divergent.wrap(39)[0] + 3)
