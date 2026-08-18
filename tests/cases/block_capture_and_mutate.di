# A block captures and mutates an enclosing local -- must go through the
# same BOX_LOCAL/SET_CELL path an ordinary nested-`def` capture uses (see
# compound_assignment_closure_capture.di), not a plain-register MOVE, and
# the mutation must be visible to the caller after the block returns.
def sum_via_block(values)
  total = 0
  values.each() do |x|
    total = total + x
  end
  total
end
sum_via_block([1, 2, 3, 4, 5])
