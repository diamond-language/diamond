def inner(n) = n * 2
def outer(n)
  t = Thread.new(inner, n)
  t.join()
end
t = Thread.new(outer, 10)
t.join()
