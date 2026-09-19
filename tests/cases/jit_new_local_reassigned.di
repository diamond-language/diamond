# A NEW-constructed local reassigned anywhere in the body (even to
# another instance of the exact same class) fails register_new_class_if_
# sole_writer's whole-body "exactly one write" rule and must still bail --
# confirms the proof is real, not skipped. Only `initialize`/`double`
# compile (2, not 3); `run_loop` (the reassigning caller) never becomes
# eligible, so 0 counted bailouts either (see jit_invoke_self_mixed_
# receiver.di's own comment on why a function that never becomes eligible
# isn't a "bailout" in DIAMOND_TRACE_JIT's own sense).
class Box
  def initialize(value)
    @value = value
  end
  def double() -> Int = @value * 2
end

def run_loop(n) -> Int
  total = 0
  i = 0
  while i < n
    box = Box.new(21)
    total = total + box.double()
    if i == 3
      box = Box.new(99)
    end
    i = i + 1
  end
  total
end

puts(run_loop(6))
