# JIT Phase 12 (docs/internal/jit-design.md): DIAMOND_OP_INVOKE joins NEW
# (Phase 10) and GET_IVAR (Phase 11) as a third class-producing terminal
# register_new_class_or_move_src recognizes -- closing the roadmap's own
# named "a method call's own return value" INVOKE-receiver gap, for the
# slice where the compiler's own real type-checking (not an LSP
# heuristic) already resolves the call's target and its *declared*
# return type: `f.make()` here, `f` a typed, never-reassigned parameter
# (Phase 9's own proof), `make()` declared `-> Widget`. Before this
# phase, `x.double()` made `run` bail whole (an unproven-type INVOKE
# receiver); both functions now compile.
class Widget
  def initialize(n)
    @n = n
  end
  def double() -> Int = @n * 2
end

class Factory
  def make(box: Widget) -> Widget = box
end

def run(f: Factory, box: Widget, n) -> Int
  x = f.make(box)
  total = 0
  i = 0
  while i < n
    total = total + x.double()
    i = i + 1
  end
  total
end

puts(run(Factory.new(), Widget.new(21), 50000))
