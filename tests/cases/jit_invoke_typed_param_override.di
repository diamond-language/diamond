# A typed parameter declared as the BASE class (`box: Box`) but holding a
# real subclass instance at runtime (`TripleBox < Box`, overriding
# `double`) must still dispatch to the override -- confirms this phase
# reuses diamond_jit_invoke_instance's own real lookup_method-gated
# dispatch (same as Phase 7's OverriddenBox test), not some incorrect
# compile-time devirtualization to Box#double just because the static
# parameter type says Box. 84 (42*2 via the override), not 42 (a wrongly
# devirtualized doubling of 21).
class Box
  def initialize(value)
    @value = value
  end
  def double() -> Int = @value * 2
end

class TripleBox < Box
  def double() -> Int = @value * 4
end

def run_loop(box: Box, n) -> Int
  total = 0
  i = 0
  while i < n
    total = total + box.double()
    i = i + 1
  end
  total
end

b = TripleBox.new(21)
puts(run_loop(b, 1))
