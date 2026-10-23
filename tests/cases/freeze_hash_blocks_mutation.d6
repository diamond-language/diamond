h = {"a": 1}
h.freeze()
puts(h.frozen?())
begin
  h["b"] = 2
rescue error: FrozenError
  puts("[]= blocked")
end
puts(h.length())
puts(h["a"])
puts(h.keys())
