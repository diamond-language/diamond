# Monomorphic dynamic dispatch -- a single receiver class at one call
# site, the case Diamond's inline caches are designed to specialize.
class Counter
  def value()
    42
  end
end

def run()
  counter = Counter.new()
  index = 0
  result = 0
  while index < 2000000
    result = counter.value()
    index = index + 1
  end
  result
end
run()
