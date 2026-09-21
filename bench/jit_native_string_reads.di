# Measures allocation-free String dispatch added to JIT Phase 17.
def read_string(value: String, needle: String, iterations: Int) -> Int
  i = 0
  total = 0
  while i < iterations
    total = total + value.length() + value.index_of(needle) + value.ord()
    i = i + 1
  end
  total
end

puts(read_string("skindicate-root-page", "root", 5000000))
