# An omitted default parameter is re-defaulted on every in-place iteration,
# not inherited from the original call's argument list.
def f(n, acc = 100)
  return acc if n == 0
  return f(n - 1)
end

def g(n, acc = 100)
  return acc if n == 0
  return g(n - 1, acc + 1)
end

puts(f(3, 5))
puts(f(3))
puts(g(3))
puts(f(200000, 7))
