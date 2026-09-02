# Two different self-referential Arrays must compare unequal without
# hanging/stack-overflowing (DIAMOND_STRUCTURAL_MAX_DEPTH bounds the
# recursion) -- and a genuinely deep but non-cyclic structure, well
# within that bound, must still compare correctly.
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
