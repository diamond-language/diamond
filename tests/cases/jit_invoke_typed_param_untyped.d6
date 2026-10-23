# An untyped parameter's parameter_type_sets slot is DIAMOND_NO_TYPE_SET
# (compile_definition's own zero-init, never overwritten since no `: Type`
# annotation was parsed), so parameter_is_single_class rejects it
# outright -- no compile-time proof exists at all. `run_loop` never
# compiles; `Box#double`/`Box#initialize` still do (2, not 3).
class Box
  def initialize(value)
    @value = value
  end
  def double() -> Int = @value * 2
end

def run_loop(box, n) -> Int
  total = 0
  i = 0
  while i < n
    total = total + box.double()
    i = i + 1
  end
  total
end

b = Box.new(21)
puts(run_loop(b, 50000))
