# A plain CALL from compiled code runs the callee as native code too: results,
# argument defaults, nested allocation and deep recursion all match the
# interpreter.
def fib(n)
  if n < 2
    n
  else
    fib(n - 1) + fib(n - 2)
  end
end

def label(prefix, n, suffix = "!") = prefix + n.to_s() + suffix

def build(count)
  items = []
  index = 0
  while index < count
    items = items + [label("item-", index)]
    index = index + 1
  end
  items
end

def total(count)
  parts = build(count)
  parts.length() + fib(15)
end

puts(fib(20))
puts(label("a", 1))
puts(label("a", 2, "?"))
puts(total(40))
puts(build(3))
