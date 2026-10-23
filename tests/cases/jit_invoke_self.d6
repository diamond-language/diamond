# Phase 7: self.method() dispatch. Before this phase, ANY DIAMOND_OP_INVOKE
# in a function's body (dynamic method dispatch, including a plain
# self.other_method() call) bailed the whole function out of JIT
# eligibility -- confirmed via git stash against this exact file: 2 compiled
# function(s) before, 4 after (bump/run_loop both newly compile).
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

c = Counter.new(41)
puts(c.run_loop(50000))
