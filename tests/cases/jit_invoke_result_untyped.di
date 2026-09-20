# `make` here has no explicit `-> Type` return annotation -- its return
# type is inferred-only, which publish_call_return_type deliberately
# routes to compiler->tooling_type_sets (LSP-only) instead of known_
# types/known_type_sets, "never known_type_sets (and therefore never
# type checks or opcode choice)" per that function's own comment. `x`'s
# register_known_class must stay UINT8_MAX; `run` correctly never
# becomes eligible.
class Widget
  def initialize(n)
    @n = n
  end
  def double() -> Int = @n * 2
end

class Factory
  def make() = Widget.new(21)
end

def run(f: Factory, n) -> Int
  x = f.make()
  total = 0
  i = 0
  while i < n
    total = total + x.double()
    i = i + 1
  end
  total
end

puts(run(Factory.new(), 50000))
