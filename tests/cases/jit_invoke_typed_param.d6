# JIT Phase 9 (docs/internal/jit-design.md): DIAMOND_OP_INVOKE support for a
# non-self receiver that's a declared parameter with a single concrete
# class type, never reassigned in the function body. `run_loop` here is a
# PLAIN top-level function (owner_class==UINT8_MAX, no self at all -- the
# self_offset==0 path in parameter_is_single_class), calling `box.double()`
# every iteration where `box` is its own first parameter -- before this
# phase, any non-self INVOKE bailed the whole containing function, so
# `run_loop` never compiled at all. `initialize`/`double` also compile
# (Phase 2g GET_IVAR / arithmetic), so all 3 defs compile now, 0 bailouts.
class Box
  def initialize(value)
    @value = value
  end
  def double() -> Int = @value * 2
end

def run_loop(box: Box, n) -> Int
  total = 0
  i = 0
  while i < n
    total = total + box.double()
    i = i + 1
  end
  total
end

b = Box.new(21)
puts(run_loop(b, 50000))
