def square(x) = x * x
def apply(f, x) = f(x)
t = Thread.new(apply, square, 6)
t.join()
