# A block capturing more than 32 enclosing locals (33 here) used to fail to compile
# ("block sees too many lexical bindings"), because a closure held a fixed array of 32 captures.
# Closures now size their captures to what they hold, so this sums all of them.
def too_many_captures()
  a = 1;  b = 1;  c = 1;  d = 1;  e = 1;  f = 1;  g = 1;  h = 1
  i = 1;  j = 1;  k = 1;  l = 1;  m = 1;  n = 1;  o = 1;  p = 1
  q = 1;  r = 1;  s = 1;  t = 1;  u = 1;  v = 1;  w = 1;  x2 = 1
  y = 1;  z = 1;  aa = 1; bb = 1; cc = 1; dd = 1; ee = 1; ff = 1
  gg = 1
  [0].map() do |n0|
    n0 + a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p +
      q + r + s + t + u + v + w + x2 + y + z + aa + bb + cc + dd + ee + ff + gg
  end
end
too_many_captures()
