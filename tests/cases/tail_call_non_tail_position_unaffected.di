def recurse_forever(n)
  1 + recurse_forever(n + 1)
end
recurse_forever(0)
