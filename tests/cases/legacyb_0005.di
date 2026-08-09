def factorial(n)
  if n <= 1
    1
  else
    n * factorial(n - 1)
  end
end

def add(a, b)
  a + b
end

add(factorial(5), -78)
