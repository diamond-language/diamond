def double(value)
  value * 2
end

h = {"a": 1, "b": 2}
puts(h.fetch("a", 0))
puts(h.fetch("z", 42))
puts(h.keys())
puts(h.values())
puts(h.include_key?("a"))
puts(h.include_key?("z"))
puts(h.map_values(double))
puts(h.merge({"b": 20, "c": 3}))
puts({}.empty?())
puts(h.empty?())
