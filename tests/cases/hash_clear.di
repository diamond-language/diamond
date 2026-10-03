# Hash#clear removes every pair, returns the same (now empty) Hash, and leaves
# it fully usable afterwards.
def clear_typed(h: Hash) -> Hash
  h.clear()
end

a = {"k": 1}
puts(clear_typed(a).length())

# Values removed from the Hash are not destroyed: only the references go.
b = {"p": {"q": 1}, "r": [1, 2]}
inner = b["p"]
b.clear()
puts(inner["q"])
puts(b.length())
puts(b.empty?())
puts(b.include_key?("p"))

# The return value is the receiver itself.
c = {"a": 1}
d = c.clear()
d["z"] = 9
puts(c.length())
puts(c["z"])

# Reuse after clearing, including re-adding a removed key.
e = {"a": 1, "b": 2, "c": 3}
e.clear()
e["b"] = 20
e["a"] = 10
puts(e.length())
puts(e["a"] + e["b"])
puts(e.keys().join(","))

# Clearing an empty Hash is a no-op that still returns it.
puts({}.clear().length())

# A large Hash clears in one pass and stays usable.
big = {}
i = 0
while i < 100000
  big["k#{i}"] = i
  i += 1
end
big.clear()
puts(big.length())
big["again"] = 1
puts(big["again"])
