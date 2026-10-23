# An ivar holding a subclass instance that overrides the method being
# called -- confirms this phase reuses diamond_jit_invoke_instance's own
# real lookup_method-gated dispatch (same as Phase 7/9/10's own override
# tests), not some incorrect compile-time devirtualization to the
# ivar_known_class's own base-class method just because that's the
# compile-time-known class. 84 (21*4 via the override), not 42 (a wrongly
# non-overridden doubling).
class Box
  def initialize(value)
    @value = value
  end
  def double() -> Int = @value * 2
end

class TripleBox < Box
  def double() -> Int = @value * 4
end

class Holder
  def initialize(box: Box)
    @box = box
  end
  def run() -> Int
    x = @box
    x.double()
  end
end

puts(Holder.new(TripleBox.new(21)).run())
