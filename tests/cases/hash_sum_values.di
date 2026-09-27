# Hash#sum adds the values; with a (key, value) block, what it returns.
h = {"a": 1, "b": 2}
[h.sum(), h.sum() do |k, v| v * 10 end]
