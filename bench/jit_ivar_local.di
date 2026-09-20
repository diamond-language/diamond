# JIT Phase 11 (docs/internal/jit-design.md): DIAMOND_OP_GET_IVAR joins
# DIAMOND_OP_NEW as a second class-producing terminal register_new_class_
# or_move_src recognizes -- `x = @box; ...; x.double()` inside a loop,
# the ivar-load counterpart to bench/jit_new_local.di's own NEW-based
# shape. Before this phase, `x.double()` here made run_loop() bail the
# whole loop out of JIT eligibility (an unproven-type INVOKE receiver);
# `box: Box` must stay typed on Holder#initialize -- an untyped parameter
# assignment can't be proven to hold a single concrete class, and this
# phase's own eligibility fix is specifically about *provable* facts, not
# a broader devirtualization.
#
# Not part of the automatic default-vs-quicken sweep bench/run.sh runs --
# DIAMOND_JIT is a separate opt-in tier, so compare by hand:
#   ./build/diamond bench/jit_ivar_local.di
#   DIAMOND_JIT=1 ./build/diamond bench/jit_ivar_local.di
class Box
  def initialize(n)
    @n = n
  end
  def double() -> Int = @n * 2
end

class Holder
  def initialize(box: Box)
    @box = box
  end
  def run_loop(iterations) -> Int
    total = 0
    i = 0
    x = @box
    while i < iterations
      total = x.double()
      i = i + 1
    end
    total
  end
end

def run()
  Holder.new(Box.new(21)).run_loop(3000000)
end
run()
