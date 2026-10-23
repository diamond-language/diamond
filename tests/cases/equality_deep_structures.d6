# == on Arrays and Hashes is exact at any nesting depth: it falls back from
# recursion to an explicit heap stack, so depth is bounded by memory and not
# the C stack, and equal deep values are never reported unequal. Structures
# that contain themselves compare by shape instead of recursing forever.
def chain(levels, bottom)
  value = bottom
  levels.times() do |step|
    value = [value]
  end
  value
end

def hash_chain(levels, bottom)
  value = bottom
  levels.times() do |step|
    value = {"next": value}
  end
  value
end

[10, 255, 256, 257, 5000, 150000].each() do |levels|
  puts("#{levels}: equal=#{chain(levels, 1) == chain(levels, 1)} differ_at_bottom=#{chain(levels, 1) == chain(levels, 2)} hashes=#{hash_chain(levels, 1) == hash_chain(levels, 1)} hash_differ=#{hash_chain(levels, 1) == hash_chain(levels, 2)}")
end

puts("depth mismatch: #{chain(300, 1) == chain(301, 1)}")
puts("!= on equal deep: #{chain(300, 1) != chain(300, 1)}")

mixed_a = []
mixed_b = []
300.times() do |i|
  mixed_a = i % 2 == 0 ? [mixed_a, {"k": i}] : {"v": mixed_a}
  mixed_b = i % 2 == 0 ? [mixed_b, {"k": i}] : {"v": mixed_b}
end
puts("mixed: #{mixed_a == mixed_b}")

lookup = {}
lookup[chain(400, "key")] = "found"
puts("deep key lookup: #{lookup[chain(400, "key")]}")
puts("deep key miss: #{lookup[chain(400, "other")]}")

a = []
a.push(a)
b = []
b.push(b)
puts("cyclic same shape: #{a == b}")
c = []
c.push([c])
puts("cyclic unrolled: #{a == c}")
d = [[]]
puts("cyclic vs finite: #{a == chain(1000, [])}")
h = {}
h["self"] = h
g = {}
g["self"] = g
puts("cyclic hash: #{h == g}")
