# A call that ends an if/else or case branch at the end of a function is a
# tail call even without `return`, so a cycle through such branches needs no stack.

def ev(n)
  if n == 0
    true
  else
    od(n - 1)
  end
end

def od(n)
  if n == 0
    false
  else
    ev(n - 1)
  end
end

puts(ev(1000000))
puts(ev(1000001))

def countdown(n, acc)
  case n
  when 0
    acc
  else
    countdown(n - 1, acc + 1)
  end
end

puts(countdown(1000000, 0))

# The value still flows through when the call is not the branch's last value.
def wrapped(n)
  if n == 0
    0
  else
    wrapped(n - 1) + 1
  end
end

puts(wrapped(50))

# A call whose result is used after the branch is an ordinary call.
def after(n)
  r = if n == 0
    0
  else
    after(n - 1)
  end
  r + 1
end

puts(after(50))
