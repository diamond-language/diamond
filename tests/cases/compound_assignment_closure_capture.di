# Compound assignment on a captured local -- must go through the same
# BOX_LOCAL/SET_CELL path an ordinary `n = n + 1` reassignment of a
# captured local already uses (compile_assignment_store's `captured`
# branch), not the plain-register MOVE path.
def make_counter()
  n = 0
  def increment()
    n += 1
    n
  end
  increment
end
counter = make_counter()
[counter(), counter(), counter()]
