# A compiled function's tail call to a different function continues in the
# interpreter's own constant-stack way: mutual and three-way cycles run deep,
# heap arguments survive the hop, and errors raised mid-cycle propagate.
def even?(n: Int) -> Bool
  return true if n == 0
  return odd?(n - 1)
end

def odd?(n: Int) -> Bool
  return false if n == 0
  return even?(n - 1)
end

puts(even?(300001))
puts(odd?(300001))

def ping(n, items)
  return items.length() if n == 0
  return pong(n - 1, items + [n])
end

def pong(n, items)
  return items.length() if n == 0
  return ring(n - 1, items)
end

def ring(n, items)
  return items.length() if n == 0
  return ping(n - 1, items)
end

puts(ping(30001, []))

def label(n, text)
  return text if n == 0
  return label_two(n - 1, text + "a")
end

def label_two(n, text)
  return text if n == 0
  return label(n - 1, text + "b")
end

puts(label(9, ""))

def strict(n: Int, tag: String) -> Int
  return n if tag == "done"
  return strict_next(n + 1, tag)
end

def strict_next(n: Int, tag: String) -> Int
  return strict(n, "done") if n > 3
  return strict(n + 1, tag)
end

puts(strict(0, "go"))

def fail_a(n)
  raise ArgumentError.new("boom at " + n.to_s()) if n == 0
  return fail_b(n - 1)
end

def fail_b(n)
  return fail_a(n - 1)
end

begin
  fail_a(1000)
rescue error: ArgumentError
  puts(error.message())
end

def defaulted(n, step = 2)
  return n if n > 20
  return defaulted_two(n + step)
end

def defaulted_two(n)
  return defaulted(n)
end

puts(defaulted(0))

def wrong_type(n: Int)
  return n if n > 5
  return wrong_type_two(n + 1)
end

def wrong_type_two(n)
  return wrong_type(n == 6 ? "x" : n)
end

begin
  wrong_type(0)
rescue error: TypeError
  puts("type error")
end
