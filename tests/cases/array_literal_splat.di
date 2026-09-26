# One *splat per array literal splices an Array's elements in place.
rest = [2, 3]
copy = [*rest]
copy.push(99)
[[1, *rest], [*rest, 4], [0, *rest, 9], [*[]], copy, rest]
