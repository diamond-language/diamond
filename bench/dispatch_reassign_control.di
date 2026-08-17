# Control for dispatch_polymorphic_no_index.di: identical control-flow
# shape (if/elsif chain + local-variable reassignment before the one
# call site) but all four locals are instances of the SAME class, so
# the inline cache at `receiver.value()` sees one class every time and
# stays on the monomorphic fast path throughout. Isolates the cost of
# the if/elsif+MOVE machinery itself from the cost of the receiver
# class actually varying -- subtract this from
# dispatch_polymorphic_no_index.di's time to get a cleaner read on
# what the polymorphic cache itself costs.
class A
  def value()
    1
  end
end

def run()
  a = A.new()
  b = A.new()
  c = A.new()
  d = A.new()
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
