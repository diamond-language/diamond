# A NEW-constructed local of a subclass that overrides the method being
# called -- confirms this phase reuses diamond_jit_invoke_instance's own
# real lookup_method-gated dispatch (same as Phase 7/9's own override
# tests), not some incorrect compile-time devirtualization to the base
# class's own method just because the NEW instruction's own compile-time
# class index names the subclass correctly (it does; the point is dispatch
# still resolves dynamically from there, not statically). 84 (21*4 via
# the override), not 42 (a wrongly non-overridden doubling).
class Box
  def initialize(value)
    @value = value
  end
  def double() -> Int = @value * 2
end

class TripleBox < Box
  def double() -> Int = @value * 4
end

def run_once() -> Int
  box = TripleBox.new(21)
  box.double()
end

puts(run_once())
