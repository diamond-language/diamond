def abs(x: Int | Float) -> Int | Float
  if x < 0
    -x
  else
    x
  end
end

def min(a: Int | Float, b: Int | Float) -> Int | Float
  if a < b
    a
  else
    b
  end
end

def max(a: Int | Float, b: Int | Float) -> Int | Float
  if a > b
    a
  else
    b
  end
end

def mod(a: Int | Float, b: Int | Float) -> Int | Float
  quotient = a / b
  if quotient is Int
    a - quotient * b
  else
    a - to_f(to_i(quotient)) * b
  end
end

# n.times() / a.upto(b) / a.downto(b) helpers.
def integer_times(n: Int, callback: Callable[1]) -> Int
  i = 0
  while i < n
    callback(i)
    i += 1
  end
  n
end

def integer_upto(start: Int, stop: Int, callback: Callable[1]) -> Int
  i = start
  while i <= stop
    callback(i)
    i += 1
  end
  start
end

def integer_downto(start: Int, stop: Int, callback: Callable[1]) -> Int
  i = start
  while i >= stop
    callback(i)
    i -= 1
  end
  start
end

def array_sort(values: Array[Int]) -> Array[Int]
  result = []
  index = 0
  while index < values.length()
    result.push(values[index])
    index += 1
  end
  i = 1
  while i < result.length()
    key = result[i]
    j = i - 1
    while j >= 0 && result[j] > key
      result[j + 1] = result[j]
      j -= 1
    end
    result[j + 1] = key
    i += 1
  end
  result
end
