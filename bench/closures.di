# Nested closures capturing and mutating an outer local in a loop --
# exercises BOX_LOCAL/GET_CELL/SET_CELL and closure allocation
# (CLOSURE opcode), a documented subtle area (a real closure-capture
# compiler bug was found and fixed earlier in this project's history)
# and a plausible target for future specialization.
def make_accumulator()
  total = 0
  def add(value)
    total = total + value
  end
  add
end

def run()
  accumulate = make_accumulator()
  index = 0
  while index < 1000000
    accumulate(1)
    index = index + 1
  end
  index
end
run()
