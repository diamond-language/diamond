# Polymorphic dynamic dispatch -- alternating receiver classes at one
# call site, the case that stresses inline cache misses/rewrites
# instead of the monomorphic fast path.
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
  objects = [A.new(), B.new(), C.new(), D.new()]
  index = 0
  result = 0
  while index < 2000000
    receiver = objects[index - (index / 4) * 4]
    result = receiver.value()
    index = index + 1
  end
  result
end
run()
