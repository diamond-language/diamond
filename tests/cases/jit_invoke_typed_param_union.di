# A union-typed parameter (`Box | Nil` -- Diamond has no `Type?` sugar,
# see the compiler's own type-annotation parsing) has type_set.count!=1,
# so parameter_is_single_class rejects it unconditionally -- correct,
# since a nil receiver must still raise through the interpreter, not hit
# diamond_jit_invoke_instance's Instance-only trampoline. `run_loop` never
# compiles; `initialize`/`double` still do (2, not 3).
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
