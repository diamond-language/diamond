# JIT Phase 10 (docs/internal/jit-design.md): DIAMOND_OP_NEW support, plus
# a later .method() call on the freshly-constructed local (never
# reassigned) dispatching through the same shared trampoline Phase 7/9
# already use. Before this phase, ANY function containing a `.new()` call
# bailed out of JIT eligibility entirely, regardless of what happened to
# the constructed value afterward -- so run_loop here never compiled at
# all. The compiler emits NEW into its own temp register, then a separate
# MOVE into `box`'s real register (confirmed via --dump-bytecode before
# writing register_new_class_or_move_src's own MOVE-chasing logic), so
# this also exercises that indirection, not just a direct NEW-to-INVOKE
# shape.
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
    i = i + 1
  end
  total
end

puts(run_loop(50000))
