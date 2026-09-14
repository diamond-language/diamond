def recurse_forever(n, *rest)
  return recurse_forever(n + 1, n)
end
recurse_forever(0)
