# JIT Phase 15 (docs/internal/jit-design.md): the actual Arel-shaped
# reproduction -- the SAME receiver register narrowed to DIFFERENT
# classes at DIFFERENT call sites, each calling a DIFFERENT method. A
# position-insensitive design (reusing register_known_class, "must agree
# everywhere") would fail this case outright, since the three sites
# disagree with each other -- this is exactly why Phase 15 needed a new
# per-call-site fact instead. Each of A/B/C's own method is independently
# resolved and dispatched correctly.
class A
  def value() -> Int
    1
  end
end

class B
  def value() -> Int
    2
  end
end

class C
  def value() -> Int
    3
  end
end

def render(x: A | B | C) -> Int
  if x is A
    x.value()
  elsif x is B
    x.value()
  elsif x is C
    x.value()
  else
    0
  end
end

total = 0
i = 0
while i < 5
  total = total + render(A.new()) + render(B.new()) + render(C.new())
  i = i + 1
end
puts(total)
