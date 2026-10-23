# A block capturing more than 16 enclosing locals used to fail to
# compile at all ("block sees too many lexical bindings"), a hardcoded
# compiler-side 16 (see DIAMOND_MAX_CAPTURES, src/object.h, now 32).
# 20 locals here (21 counting `total`, which the block also captures
# to accumulate into) -- summed inside one block, in a real closure-
# over-a-block-parameter shape, to prove every single one actually
# made it through correctly, not just that compilation succeeded.
def sum_twenty_locals()
  a = 1
  b = 2
  c = 3
  d = 4
  e = 5
  f = 6
  g = 7
  h = 8
  i = 9
  j = 10
  k = 11
  l = 12
  m = 13
  n = 14
  o = 15
  p = 16
  q = 17
  r = 18
  s = 19
  t = 20
  total = 0
  [0].each() do |x|
    total = a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p + q + r + s + t + x
  end
  total
end
sum_twenty_locals()
