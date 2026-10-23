# An object pattern that only binds (or ignores) its readers matches every
# instance of its class, so it covers that class for exhaustiveness.
sealed class Result
end
class Ok < Result
  attr_reader value: Int
  def initialize(value: Int)
    @value = value
  end
end
class Failed < Result
  attr_reader reason: String
  def initialize(reason: String)
    @reason = reason
  end
end
def describe(result: Result) -> String
  case result
  when Ok{value: value} then "ok #{value}"
  when Failed{reason: _} then "failed"
  end
end
def total() -> Int
  sum = 0
  [1, 2, 3].each() do |x|
    next if x == 2
    sum += x
  end
  sum
end
[describe(Ok.new(1)), describe(Failed.new("x")), total()]
