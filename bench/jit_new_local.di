# JIT Phase 10 (docs/internal/jit-design.md): DIAMOND_OP_NEW support, plus
# a later .method() call on the freshly-constructed local (never
# reassigned) dispatching through the same shared trampoline Phase 7/9
# already use. Before this phase, ANY function containing a `.new()` call
# bailed the whole containing function out of JIT eligibility outright, so
# run_loop() here -- constructing a fresh Box every iteration and calling
# a method on it, an ordinary allocation-in-a-loop shape -- never compiled
# at all. Unlike bench/jit_dup.di's own driver, THIS loop itself is
# expected to fully compile now (box's own register_new_class_if_sole_
# writer proof, chasing through the compiler's own NEW-then-MOVE-into-
# local-register shape, satisfies compile_body's new gate), so this
# measures a genuine whole-loop win, not just construction or dispatch in
# isolation.
#
# Not part of the automatic default-vs-quicken sweep bench/run.sh runs --
# DIAMOND_JIT is a separate opt-in tier, so compare by hand:
#   ./build/diamond bench/jit_new_local.di
#   DIAMOND_JIT=1 ./build/diamond bench/jit_new_local.di
class Box
  def initialize(n)
    @n = n
  end
  def double() -> Int = @n * 2
end

def run_loop(iterations) -> Int
  total = 0
  i = 0
  while i < iterations
    box = Box.new(i)
    total = box.double()
    i = i + 1
  end
  total
end

def run()
  run_loop(3000000)
end
run()
