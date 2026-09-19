# A typed parameter reassigned anywhere in the body (even to another
# in-scope instance of the exact same class, via a plain MOVE -- not a
# DIAMOND_OP_NEW, which the JIT doesn't support at all and would bail the
# function for an unrelated reason) fails parameter_never_reassigned's
# whole-body scan and must still bail -- confirms the "never reassigned"
# proof is real, not skipped. Only `initialize`/`double` compile (2, not
# 3); `run_loop` (the reassigning caller) never becomes eligible, so 0
# counted bailouts either (see jit_invoke_self_mixed_receiver.di's own
# comment on why a function that never becomes eligible isn't a
# "bailout" in DIAMOND_TRACE_JIT's own sense).
class Box
  def initialize(value)
    @value = value
  end
  def double() -> Int = @value * 2
end

def run_loop(box: Box, other_box: Box, n) -> Int
  total = 0
  i = 0
  while i < n
    total = total + box.double()
    if i == 3
      box = other_box
    end
    i = i + 1
  end
  total
end

b = Box.new(21)
other = Box.new(99)
puts(run_loop(b, other, 6))
