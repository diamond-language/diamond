# Mutual and cross-function tail calls: a call in tail position to a function that
# can tail-call back runs in place, so the cycle needs no stack. A tail call to a
# function that is not on such a cycle stays an ordinary call.

def even?(n, acc = 0)
  if n == 0
    return true
  end
  return odd?(n - 1)
end

def odd?(n)
  if n == 0
    return false
  end
  return even?(n - 1)
end

puts(even?(1000001))
puts(even?(1000000))

# three-way cycle with different arities and a default
def a(n, total)
  return total if n == 0
  b(n - 1, total + 1, 2)
end
def b(n, total, step) = c(n, total + step)
def c(n, total) = a(n, total)
puts(a(300000, 0))

# wrapper, not a cycle
def wrap(x) = inner(x + 1)
def inner(x) = x * 2
puts(wrap(4))

# big register file in one function of the cycle
def big(n)
  v0 = n; v1 = v0 + 1; v2 = v1 + 1; v3 = v2 + 1; v4 = v3 + 1; v5 = v4 + 1
  return v5 if n >= 200000
  small(n + 1)
end
def small(n) = big(n)
puts(big(0))

def fat(n)
  xs = [n + 0, n + 1, n + 2, n + 3, n + 4, n + 5, n + 6, n + 7, n + 8, n + 9, n + 10, n + 11, n + 12, n + 13, n + 14, n + 15, n + 16, n + 17, n + 18, n + 19, n + 20, n + 21, n + 22, n + 23, n + 24, n + 25, n + 26, n + 27, n + 28, n + 29, n + 30, n + 31, n + 32, n + 33, n + 34, n + 35, n + 36, n + 37, n + 38, n + 39, n + 40, n + 41, n + 42, n + 43, n + 44, n + 45, n + 46, n + 47, n + 48, n + 49, n + 50, n + 51, n + 52, n + 53, n + 54, n + 55, n + 56, n + 57, n + 58, n + 59, n + 60, n + 61, n + 62, n + 63, n + 64, n + 65, n + 66, n + 67, n + 68, n + 69, n + 70, n + 71, n + 72, n + 73, n + 74, n + 75, n + 76, n + 77, n + 78, n + 79, n + 80, n + 81, n + 82, n + 83, n + 84, n + 85, n + 86, n + 87, n + 88, n + 89, n + 90, n + 91, n + 92, n + 93, n + 94, n + 95, n + 96, n + 97, n + 98, n + 99, n + 100, n + 101, n + 102, n + 103, n + 104, n + 105, n + 106, n + 107, n + 108, n + 109, n + 110, n + 111, n + 112, n + 113, n + 114, n + 115, n + 116, n + 117, n + 118, n + 119]
  ys = [n + 0, n + 1, n + 2, n + 3, n + 4, n + 5, n + 6, n + 7, n + 8, n + 9, n + 10, n + 11, n + 12, n + 13, n + 14, n + 15, n + 16, n + 17, n + 18, n + 19, n + 20, n + 21, n + 22, n + 23, n + 24, n + 25, n + 26, n + 27, n + 28, n + 29, n + 30, n + 31, n + 32, n + 33, n + 34, n + 35, n + 36, n + 37, n + 38, n + 39, n + 40, n + 41, n + 42, n + 43, n + 44, n + 45, n + 46, n + 47, n + 48, n + 49, n + 50, n + 51, n + 52, n + 53, n + 54, n + 55, n + 56, n + 57, n + 58, n + 59, n + 60, n + 61, n + 62, n + 63, n + 64, n + 65, n + 66, n + 67, n + 68, n + 69, n + 70, n + 71, n + 72, n + 73, n + 74, n + 75, n + 76, n + 77, n + 78, n + 79, n + 80, n + 81, n + 82, n + 83, n + 84, n + 85, n + 86, n + 87, n + 88, n + 89, n + 90, n + 91, n + 92, n + 93, n + 94, n + 95, n + 96, n + 97, n + 98, n + 99, n + 100, n + 101, n + 102, n + 103, n + 104, n + 105, n + 106, n + 107, n + 108, n + 109, n + 110, n + 111, n + 112, n + 113, n + 114, n + 115, n + 116, n + 117, n + 118, n + 119]
  return xs.length() + ys.length() + n if n >= 100000
  lean(n + 1)
end
def lean(n) = fat(n)
puts(fat(0))

def morph_a(x, n)
  return x if n == 0
  morph_b(x.to_s(), n - 1)
end
def morph_b(x, n) = morph_a(x.length(), n - 1)
puts(morph_a(5, 200000))

def gen_a[T](x: T, n: Int) -> String
  return x.to_s() if n == 0
  return gen_b(x.to_s(), n - 1)
end
def gen_b[T](x: T, n: Int) -> String
  return gen_a(x, n - 1)
end
puts(gen_a(7, 200000))
puts(gen_a("s", 200000))

# Overflow arguments (the *rest array) are re-collected on each switch.
def va(n, *rest)
  return rest.length() if n == 0
  vb(n - 1, "x#{n}", "y", "z", 7, 8, 9, 10)
end
def vb(n, *rest)
  va(n, "p#{n}", "q", "r", "s")
end
puts(va(100000, 1, 2))

def da(n, acc = 100)
  return acc if n == 0
  db(n - 1)
end
def db(n, acc = 7) = da(n, acc + 1)
puts(da(5, 0))

# a lambda caller falls back to an ordinary call
def ping(n)
  return n if n <= 0
  pong(n - 1)
end
def pong(n) = ping(n - 1)
r = [11, 12].map() do |k|
  ping(k)
end
puts(r)
puts(ping(150000))

# raising from deep inside the cycle still surfaces a clean error
def boom_a(n)
  raise ArgumentError.new("bottom") if n == 0
  boom_b(n - 1)
end
def boom_b(n) = boom_a(n)
begin
  boom_a(100000)
rescue error: ArgumentError
  puts("caught")
end

