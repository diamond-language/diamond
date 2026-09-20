# JIT Phase 15 (docs/internal/jit-design.md): the original motivating
# case Phase 14 stopped short of -- a receiver narrowed by `is` from a
# declared union parameter now chains its own method call, via a new
# position-sensitive per-call-site fact. Before this phase, run_loop()
# below never compiled at all: box's own *declared* type is a union, so
# parameter_is_single_class rejects it outright, and nothing ever writes
# to box's own register, so register_new_class_if_sole_writer can't
# prove anything about it either -- neither existing position-insensitive
# proof can ever cover this shape.
#
# Not part of the automatic default-vs-quicken sweep bench/run.sh runs --
# DIAMOND_JIT is a separate opt-in tier, so compare by hand:
#   ./build/diamond bench/jit_is_narrowed_invoke.di
#   DIAMOND_JIT=1 ./build/diamond bench/jit_is_narrowed_invoke.di
class Box
  def initialize(n)
    @n = n
  end
  def double() -> Int = @n * 2
end

def run_loop(box: Box | Nil, iterations) -> Int
  total = 0
  i = 0
  while i < iterations
    if box is Box
      total = box.double()
    end
    i = i + 1
  end
  total
end

def run()
  run_loop(Box.new(21), 3000000)
end
puts(run())
