# JIT Phase 15 (docs/internal/jit-design.md): the original motivating
# case Phase 14 stopped short of -- a receiver narrowed by `is` from a
# declared union type now chains its own method call, via a new
# position-sensitive per-call-site fact (DiamondFunction.invoke_site_
# known_class), not the position-insensitive proofs Phases 9/10/12/13
# already had (none of which can cover this: the parameter's own
# *declared* type is a union, and nothing ever writes to it, so neither
# parameter_is_single_class nor register_new_class_if_sole_writer can
# ever prove anything about it on their own).
class Widget
  def double() -> Int
    21
  end
end

class Derived
  def helper() -> Widget
    Widget.new()
  end
end

class OtherBase
  def helper() -> Widget
    Widget.new()
  end
end

def run(x: Derived | OtherBase) -> Int
  if x is Derived
    y = x.helper()
    y.double()
  else
    0
  end
end

total = 0
i = 0
while i < 5
  total = total + run(Derived.new())
  i = i + 1
end
puts(total)
