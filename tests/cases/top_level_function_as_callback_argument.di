def square(x)
  x * x
end
def apply_twice(fn, x)
  fn(fn(x))
end
apply_twice(square, 3)
