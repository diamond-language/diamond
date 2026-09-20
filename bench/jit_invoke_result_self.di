# JIT Phase 13 (docs/internal/jit-design.md, an addendum to Phase 12):
# self's own register (0) now feeds known_types the same way a typed
# parameter's already does -- `x = self.make(); ...; x.double()` inside
# a loop. Before this phase, `x.double()` here made run_loop() bail the
# whole loop out of JIT eligibility (an unproven-type INVOKE receiver)
# even though self's own class was always known.
#
# Not part of the automatic default-vs-quicken sweep bench/run.sh runs --
# DIAMOND_JIT is a separate opt-in tier, so compare by hand:
#   ./build/diamond bench/jit_invoke_result_self.di
#   DIAMOND_JIT=1 ./build/diamond bench/jit_invoke_result_self.di
class Widget
  def initialize(n)
    @n = n
  end
  def double() -> Int = @n * 2
end

class Factory
  def make() -> Widget = Widget.new(21)
  def run_loop(iterations) -> Int
    total = 0
    i = 0
    while i < iterations
      x = self.make()
      total = x.double()
      i = i + 1
    end
    total
  end
end

def run()
  Factory.new().run_loop(3000000)
end
run()
