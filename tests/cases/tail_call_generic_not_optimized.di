def recurse_forever[T](n: Int, value: T) -> T
  return recurse_forever(n + 1, value)
end
recurse_forever(0, 5)
