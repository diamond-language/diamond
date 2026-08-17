# Genuine megamorphic dispatch: 6 stable receiver classes cycling
# through one call site, deliberately exceeding DIAMOND_INLINE_CACHE_
# WIDTH (4, src/vm.h) so the cache can never hold the full working set
# and every call evicts and re-populates an entry -- the real "slow
# path on every call" case the roadmap's polymorphic-inline-cache entry
# describes, as a control against dispatch_polymorphic_no_index.di's
# genuinely-4-wide (fits-in-cache) case.
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
class E
  def value()
    5
  end
end
class F
  def value()
    6
  end
end

def run()
  a = A.new()
  b = B.new()
  c = C.new()
  d = D.new()
  e = E.new()
  f = F.new()
  index = 0
  result = 0
  while index < 2000000
    slot = index - (index / 6) * 6
    receiver = a
    if slot == 1
      receiver = b
    elsif slot == 2
      receiver = c
    elsif slot == 3
      receiver = d
    elsif slot == 4
      receiver = e
    elsif slot == 5
      receiver = f
    end
    result = receiver.value()
    index = index + 1
  end
  result
end
run()
