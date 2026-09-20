# Phase 12's other verified-working inner-receiver shape: `f` proven via
# Phase 10's own NEW-local terminal (`f = Factory.new()`), not a typed
# parameter -- confirms the new register_known_class snapshot composes
# with an *existing* class-producing terminal, not just a fresh proof of
# its own. `f.make()`'s own receiver (`f`) and `x`'s own receiver
# (`f.make()`'s result) are each proven by a different one of Phases
# 10/12's own terminals in the same function body.
class Widget
  def initialize(n)
    @n = n
  end
  def double() -> Int = @n * 2
end

class Factory
  def make() -> Widget = Widget.new(21)
end

def run(n) -> Int
  f = Factory.new()
  x = f.make()
  total = 0
  i = 0
  while i < n
    total = total + x.double()
    i = i + 1
  end
  total
end

puts(run(50000))
