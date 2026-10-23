# A generic function's own tail call runs in place and re-infers its type
# variables from the new arguments, as a fresh call would.
def morph[T](value: T, n: Int) -> String
  return value.to_s() if n == 0
  return morph(value.to_s(), n - 1)
end

puts(morph(42, 300000))

# T is Int on the first call and String afterwards: the bindings from the
# first iteration must not constrain the second.
def rewrap[T](a: T, b: T, n: Int) -> String
  return "#{a}#{b}" if n == 0
  return rewrap("x#{a}", "y#{b}", n - 1)
end

puts(rewrap(1, 2, 3))
