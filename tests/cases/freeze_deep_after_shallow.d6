# Unlike freeze, deep_freeze still descends into a value that is already
# (shallowly) frozen, and leaves immutable values alone.
x = [[1], [2]]
x.freeze()
puts(x[0].frozen?())
x.deep_freeze()
puts(x[0].frozen?())
puts([5.deep_freeze(), "s".deep_freeze()])
