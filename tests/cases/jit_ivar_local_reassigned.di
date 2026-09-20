# An ivar-loaded local reassigned anywhere in the body (even to another
# read of the exact same ivar) fails register_new_class_if_sole_writer's
# whole-body "exactly one write" rule and must still bail -- confirms the
# proof is real, not skipped, same shape as jit_new_local_reassigned.di's
# identical NEW-based test. Only Box#initialize/Holder#initialize/
# Box#double compile (3, verified by name via a temporary local trace,
# not assumed); `run` (the reassigning caller) never becomes eligible, so
# 0 counted bailouts either (a function that never becomes eligible isn't
# a "bailout" in DIAMOND_TRACE_JIT's own sense).
class Box
  def initialize(value)
    @value = value
  end
  def double() -> Int = @value * 2
end

class Holder
  def initialize(box: Box)
    @box = box
  end
  def run(n) -> Int
    x = @box
    total = 0
    i = 0
    while i < n
      total = total + x.double()
      if i == 3
        x = @box
      end
      i = i + 1
    end
    total
  end
end

puts(Holder.new(Box.new(21)).run(6))
