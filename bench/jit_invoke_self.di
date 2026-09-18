# JIT Phase 7 (docs/internal/jit-design.md): DIAMOND_OP_INVOKE/INVOKE_MONO
# support for self.method() dispatch -- any method name, not just the
# three receiver-kind-agnostic pseudo-methods bench/jit_dup.di isolates.
# Before this phase, ANY dynamic method call (including a plain
# self.other_method()) bailed the whole containing function out of JIT
# eligibility outright, so run_loop() here -- an ordinary loop calling a
# method on self every iteration, unremarkable real-world shape (e.g.
# ActiveRecord's own instance methods calling each other) -- never
# compiled at all. Unlike bench/jit_dup.di's own driver, THIS loop itself
# is expected to fully compile now (self.bump()'s own recv==0 satisfies
# compile_invoke_self's compile-time gate), so this measures a genuine
# whole-loop win, not just one isolated call's own dispatch overhead.
#
# Not part of the automatic default-vs-quicken sweep bench/run.sh runs --
# DIAMOND_JIT is a separate opt-in tier, so compare by hand:
#   ./build/diamond bench/jit_invoke_self.di
#   DIAMOND_JIT=1 ./build/diamond bench/jit_invoke_self.di
class Counter
  def initialize(n)
    @n = n
  end
  def value() = @n
  def bump() = self.value() + 1
  def run_loop(iterations)
    total = 0
    i = 0
    while i < iterations
      total = self.bump()
      i = i + 1
    end
    total
  end
end

def run()
  c = Counter.new(0)
  c.run_loop(3000000)
end
run()
