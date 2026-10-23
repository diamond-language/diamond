# JIT Phase 13 (docs/internal/jit-design.md, an addendum to Phase 12):
# `self`'s own register (0) now feeds the compiler's known_types
# tracking the same way a typed parameter's already does, closing the
# "self.method().other()" half of Phase 12's own documented gap.
# `x = self.make()` here; before this phase, `x.double()` made `run`
# bail whole (an unproven-type INVOKE receiver) even though `self`'s own
# class was always known.
class Widget
  def initialize(n)
    @n = n
  end
  def double() -> Int = @n * 2
end

class Factory
  def make() -> Widget = Widget.new(21)
  def run(n) -> Int
    x = self.make()
    total = 0
    i = 0
    while i < n
      total = total + x.double()
      i = i + 1
    end
    total
  end
end

puts(Factory.new().run(50000))
