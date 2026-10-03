# Two different self-referential Arrays must compare without
# hanging/stack-overflowing -- they have the same shape, so they are equal --
# and a genuinely deep but non-cyclic structure must still compare correctly.
a = []
a.push(a)
b = []
b.push(b)

deep_a = 0
deep_b = 0
i = 0
while i < 100
  deep_a = [deep_a]
  deep_b = [deep_b]
  i += 1
end

[a == b, a == a, deep_a == deep_b]
