# Phase 2b's actual working slice (see docs/internal/jit-design.md and
# bench/object_hydration.di's own comment on what it does NOT yet reach):
# a constructor taking a Hash argument plus a non-literal key, reading via
# INDEX_GET (a Hash-only trampoline) and writing a typed ivar via SET_IVAR
# (a shape-transition/write-barrier trampoline) -- no allocation anywhere
# in the compiled function itself, so no DiamondFrame/GC-root publishing
# is needed, unlike a realistic Hash-literal-keyed constructor would
# require (see object_hydration.di's own note on why that one still isn't
# JIT-eligible). The key is a parameter, not a literal, specifically to
# avoid the DIAMOND_OP_STRING allocation a literal key would compile to.
class Box
  attr_accessor value: Int

  def initialize(data: Hash, key)
    @value = data[key]
  end
end

def run()
  key = "x"
  total = 0
  batch = 0
  while batch < 100
    index = 0
    while index < 100
      data = {"x": index}
      box = Box.new(data, key)
      total = total + box.value()
      index = index + 1
    end
    batch = batch + 1
  end
  total
end
run()
