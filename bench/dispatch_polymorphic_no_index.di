# Polymorphic dynamic dispatch, isolated from dispatch_polymorphic.di's
# own Array-indexing confound: one call site (`receiver.value()`), fed
# by cheap local-variable reassignment (plain MOVE opcodes) instead of
# `objects[i]` (bounds-checked Array#[]) -- same call-site-alternation
# shape (4 stable classes cycling through the same INVOKE instruction,
# real inline-cache pressure), without the extra INDEX_GET opcode per
# iteration dispatch_polymorphic.di pays. See bench/BASELINE.md for why
# this variant exists: the original measured "poly tax" over
# dispatch_monomorphic.di couldn't distinguish real dispatch cost from
# this indexing overhead. (An if/elsif chain with a separate `.value()`
# call in each branch was considered and rejected -- that creates four
# separate monomorphic call sites, not one polymorphic one, since the
# inline cache is keyed by bytecode address; this design keeps exactly
# one call site.)
class A
  def value()
    1
  end
end
class B
  def value()
    2
  end
end
class C
  def value()
    3
  end
end
class D
  def value()
    4
  end
end

def run()
  a = A.new()
  b = B.new()
  c = C.new()
  d = D.new()
  index = 0
  result = 0
  while index < 2000000
    slot = index - (index / 4) * 4
    receiver = a
    if slot == 1
      receiver = b
    elsif slot == 2
      receiver = c
    elsif slot == 3
      receiver = d
    end
    result = receiver.value()
    index = index + 1
  end
  result
end
run()
