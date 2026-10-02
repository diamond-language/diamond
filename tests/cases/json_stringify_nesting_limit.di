# Nesting is bounded like JSON.parse's: 91 levels encode, deeper (and any
# self-containing collection) raises SystemStackError instead of
# recursing until the C stack overflows.
def nest(levels)
  value = []
  levels.times() do |step|
    value = [value]
  end
  value
end

puts(JSON.stringify(nest(50)).length())
puts(JSON.stringify(nest(91)).length())

def attempt(label, &block)
  begin
    yield
    puts("#{label}: ok")
  rescue e
    puts("#{label}: #{e.class()}")
  end
end

attempt("92 levels") do
  JSON.stringify(nest(92))
end
attempt("100000 levels") do
  JSON.stringify(nest(100000))
end

cyclic_array = []
cyclic_array.push(cyclic_array)
attempt("array containing itself") do
  JSON.stringify(cyclic_array)
end

cyclic_hash = {}
cyclic_hash["self"] = cyclic_hash
attempt("hash containing itself") do
  JSON.stringify(cyclic_hash)
end

# The failed attempts must leave the encoder usable.
puts(JSON.stringify([1, {"a": [2]}]))
