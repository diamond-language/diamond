# JIT Phase 12 (docs/internal/jit-design.md): DIAMOND_OP_INVOKE joins NEW
# and GET_IVAR as a third class-producing terminal -- `x = f.make(); ...;
# x.double()` inside a loop, `f` a typed parameter (Phase 9's own proof),
# `make()` declared `-> Widget`. Before this phase, `x.double()` here
# made run_loop() bail the whole loop out of JIT eligibility (an
# unproven-type INVOKE receiver).
#
# Not part of the automatic default-vs-quicken sweep bench/run.sh runs --
# DIAMOND_JIT is a separate opt-in tier, so compare by hand:
#   ./build/diamond bench/jit_invoke_result.di
#   DIAMOND_JIT=1 ./build/diamond bench/jit_invoke_result.di
class Widget
  def initialize(n)
    @n = n
  end
  def double() -> Int = @n * 2
end

class Factory
  def make(box: Widget) -> Widget = box
end

def run_loop(f: Factory, box: Widget, iterations) -> Int
  total = 0
  i = 0
  while i < iterations
    x = f.make(box)
    total = x.double()
    i = i + 1
  end
  total
end

def run()
  run_loop(Factory.new(), Widget.new(21), 3000000)
end
run()
