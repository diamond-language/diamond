# Same arithmetic as int_arithmetic.di, but routed through an untyped
# function parameter so the compiler's static known_types inference
# (src/compiler.c ~2217-2260, which already specializes ADD/SUBTRACT/
# MULTIPLY/DIVIDE to their _INT forms at compile time whenever both
# operands are locally known to be Int) cannot prove the type ahead of
# time. This is the case the *runtime* quickening tier (DIAMOND_QUICKEN)
# actually exists for -- int_arithmetic.di's locals are statically
# Int-inferable, so quickening has nothing left to do there.
def step(total, index)
  next_total = total + index
  next_total = next_total - 1
  next_total = next_total * 2
  next_total = next_total / 2
  next_total
end

def run()
  total = 0
  index = 0
  while index < 5000000
    total = step(total, index)
    index = index + 1
  end
  total
end
run()
