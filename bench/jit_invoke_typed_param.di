# JIT Phase 9 (docs/internal/jit-design.md): DIAMOND_OP_INVOKE support for a
# non-self receiver that's a declared, never-reassigned, single-concrete-
# class-typed parameter. Before this phase, ANY non-self dynamic method
# call bailed the whole containing function out of JIT eligibility
# outright (Phase 7 only closed the self-receiver slice), so run_loop()
# here -- an ordinary loop calling a method on a typed parameter every
# iteration, the exact shape Arel::Visitor's own render_attribute(
# attribute: Arel::Attribute)-style methods use -- never compiled at all.
# Unlike bench/jit_dup.di's own driver, THIS loop itself is expected to
# fully compile now (box's own parameter_is_single_class/parameter_never_
# reassigned proof satisfies compile_body's new gate), so this measures a
# genuine whole-loop win, not just one isolated call's own dispatch
# overhead.
#
# Not part of the automatic default-vs-quicken sweep bench/run.sh runs --
# DIAMOND_JIT is a separate opt-in tier, so compare by hand:
#   ./build/diamond bench/jit_invoke_typed_param.di
#   DIAMOND_JIT=1 ./build/diamond bench/jit_invoke_typed_param.di
class Box
  def initialize(n)
    @n = n
  end
  def double() -> Int = @n * 2
end

def run_loop(box: Box, iterations) -> Int
  total = 0
  i = 0
  while i < iterations
    total = box.double()
    i = i + 1
  end
  total
end

def run()
  b = Box.new(0)
  run_loop(b, 3000000)
end
run()
