h = {"a": 1, "b": 2, "c": 3}
removed = h.delete("b")
missing = h.delete("zz")
h["b"] = 9
frozen = begin
  {"x": 1}.freeze().delete("x")
rescue error: FrozenError
  "frozen"
end
[removed, missing, h.keys(), h["c"], h.length(), frozen, h.delete(key: "a")]
