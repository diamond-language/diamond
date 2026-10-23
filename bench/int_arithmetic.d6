# Tight Int arithmetic loop -- exercises ADD_INT/SUBTRACT_INT/MULTIPLY_INT
# and the runtime quickening path (DIAMOND_QUICKEN) that specializes ADD/
# SUBTRACT/MULTIPLY once operands are observed to be Int.
def run()
  total = 0
  index = 0
  while index < 5000000
    total = total + index
    total = total - 1
    total = total * 2
    total = total / 2
    index = index + 1
  end
  total
end
run()
