# A user-defined Instance method call is a native INVOKE the JIT still
# doesn't support (only dup/freeze/frozen? are, since Phase 4 -- see
# docs/internal/jit-design.md) -- this function must always fall back to
# interpretation, proving that still works correctly even under
# DIAMOND_JIT_THRESHOLD=1. `value * 2` used to be this file's own
# still-unsupported construct, until Phase 5 added JIT support for
# generic (non-typed) arithmetic and made it eligible.
class Doubler
  def double(n) = n * 2
end

def call_it(doubler, value) = doubler.double(value)

def run()
  doubler = Doubler.new()
  total = 0
  index = 0
  while index < 50
    total = total + call_it(doubler, index)
    index = index + 1
  end
  total
end
run()
