# A compiled function's own tail call is a loop: the same results as the
# interpreter, with typed parameters re-checked, defaults left to the
# interpreter, a mutual cycle still correct, and deep iteration in constant
# stack.
def sum(n, acc)
  return acc if n == 0
  return sum(n - 1, acc + n)
end

def typed(n: Int, acc: Int) -> Int
  return acc if n == 0
  return typed(n - 1, acc + 2)
end

def with_default(n, acc = 100)
  return acc if n == 0
  return with_default(n - 1, acc + 1)
end

def ev(n)
  return true if n == 0
  return od(n - 1)
end

def od(n)
  return false if n == 0
  return ev(n - 1)
end

def collatz(n, steps)
  return steps if n == 1
  return collatz(n / 2, steps + 1) if n % 2 == 0
  return collatz(3 * n + 1, steps + 1)
end

def pick(flag, n, acc)
  if n == 0
    acc
  else
    pick(!flag, n - 1, flag ? acc + 1 : acc)
  end
end

puts(sum(2000000, 0))
puts(typed(500000, 0))
puts(with_default(1000))
puts(ev(100001))
puts(collatz(27, 0))
puts(pick(true, 1000001, 0))
bad = [1, "x"][1]
begin
  typed(bad, 0)
rescue error: TypeError
  puts("type error")
end
