# A union-typed parameter (`Box | Nil` -- Diamond has no `Type?` sugar,
# see the compiler's own type-annotation parsing) has type_set.count!=1,
# so parameter_is_single_class rejects it unconditionally on its own --
# correct, since a nil receiver must still raise through the interpreter,
# not hit diamond_jit_invoke_instance's Instance-only trampoline. Before
# JIT Phase 15 (docs/internal/jit-design.md), that meant `run_loop` never
# compiled at all, even inside the `if box is Box` branch below, which
# narrows it to a single concrete class right at box.double()'s own call
# site -- Phase 15's new per-call-site fact (DiamondFunction.invoke_
# site_known_class) now covers exactly this, so `run_loop` compiles too
# (3, not 2).
class Box
  def initialize(value)
    @value = value
  end
  def double() -> Int = @value * 2
end

def run_loop(box: Box | Nil, n) -> Int
  total = 0
  i = 0
  while i < n
    if box is Box
      total = total + box.double()
    end
    i = i + 1
  end
  total
end

b = Box.new(21)
puts(run_loop(b, 50000))
