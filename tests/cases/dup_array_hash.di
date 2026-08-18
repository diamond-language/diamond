# dup produces a distinct, independently mutable Array/Hash -- mutating
# the copy must not affect the original.
a = [1, 2, 3]
b = a.dup()
b.push(4)

h = {"x": 1}
h2 = h.dup()
h2["y"] = 2

"#{a}, #{b}, #{h.length()}, #{h2.length()}"
