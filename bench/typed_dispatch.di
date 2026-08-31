# Typed-parameter/generic dispatch -- exercises CHECK_TYPE/CALL_TYPED/
# INVOKE_TYPED, a documented gap in bench/BASELINE.md's own "Coverage
# gaps" section (none of the original benchmarks showed these opcodes
# in their top-12 at all). `process` has a declared Int parameter and
# return type (checked at the call boundary); `wrap` is a generic
# method instantiated with an explicit type argument every call.
class Typed
  def process(value: Int) -> Int
    value * 2
  end
  def wrap[T](value: T) -> Array[T]
    [value]
  end
end

def run()
  receiver = Typed.new()
  total = 0
  index = 0
  while index < 1000000
    total = total + receiver.process(index)
    total = total + receiver.wrap[Int](index).length()
    index = index + 1
  end
  total
end
run()
