# A variadic function's own tail call runs in place and re-collects its
# overflow arguments from the tail call's argument list.
def g(n, stop, *rest)
  return rest if n == stop
  return g(n - 1, stop, n) if n % 2 == 0
  return g(n - 1, stop, n, n * 10)
end

puts(g(6, 5).to_s())
puts(g(5, 2).to_s())
puts(g(5, 3).to_s())
puts(g(5, 0).to_s())

# Far past the call-depth limit, with a changing overflow length.
puts(g(300001, 0).to_s())

# More overflow arguments than a function may declare, heap objects kept
# alive across collections while only the staging buffer holds them.
def wide(n, *items)
  return items.length() if n == 0
  return wide(n - 1, "a#{n}", "b#{n}", "c#{n}", "d#{n}", "e#{n}", "f#{n}", "g#{n}", "h#{n}",
              "a#{n}", "b#{n}", "c#{n}", "d#{n}", "e#{n}", "f#{n}", "g#{n}", "h#{n}",
              "a#{n}", "b#{n}", "c#{n}", "d#{n}", "e#{n}", "f#{n}", "g#{n}", "h#{n}",
              "a#{n}", "b#{n}", "c#{n}", "d#{n}", "e#{n}", "f#{n}", "g#{n}", "h#{n}",
              "a#{n}", "b#{n}", "c#{n}", "d#{n}", "e#{n}", "f#{n}", "g#{n}", "h#{n}")
end

puts(wide(50000))

def last_items(n, *items)
  return items.first() + items.last() if n == 0
  return last_items(n - 1, "first#{n}", "x", "y", "z", "last#{n}")
end

puts(last_items(100000))
