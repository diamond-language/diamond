def square(x)
  x * x
end
def apply(f, x)
  f(x)
end
t = Thread.new(apply, square, 6)
t.join()
