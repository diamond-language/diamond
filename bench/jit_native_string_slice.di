# Measures allocating String#slice added in JIT Phase 18.
def slice_many(value: String, iterations: Int) -> Int
  i = 0
  total = 0
  while i < iterations
    piece = value.slice(3, 99)
    total = total + piece.length()
    i = i + 1
  end
  total
end

puts(slice_many("skindicate-root-page", 1000000))
