# One array (or hash) handed down a chain of functions that each declare
# the same element type. Each function has its own type table, so each
# looked like a new constraint, and a container only holds four: the fifth
# function failed with "expected Array[String], got Array[String]".
def a1(items: Array[String]) -> Int = a2(items)
def a2(items: Array[String]) -> Int = a3(items)
def a3(items: Array[String]) -> Int = a4(items)
def a4(items: Array[String]) -> Int = a5(items)
def a5(items: Array[String]) -> Int = a6(items)
def a6(items: Array[String]) -> Int = a7(items)
def a7(items: Array[String]) -> Int = items.length()
puts(a1(["x", "y"]))

# The same array as several parameters of several functions at once.
def wide(a: Array[String], b: Array[String], c: Array[String]) -> Int = a.length() + b.length() + c.length()
def wider(a: Array[String], b: Array[String], c: Array[String]) -> Int = wide(a, b, c) + wide(c, b, a)
shared = ["x"]
puts(wider(shared, shared, shared))

# Hashes, and nested element types.
def h1(h: Hash[String, Int]) -> Int = h2(h)
def h2(h: Hash[String, Int]) -> Int = h3(h)
def h3(h: Hash[String, Int]) -> Int = h4(h)
def h4(h: Hash[String, Int]) -> Int = h5(h)
def h5(h: Hash[String, Int]) -> Int = h6(h)
def h6(h: Hash[String, Int]) -> Int = h.length()
puts(h1({"a": 1, "b": 2}))

def n1(rows: Array[Array[Int]]) -> Int = n2(rows)
def n2(rows: Array[Array[Int]]) -> Int = n3(rows)
def n3(rows: Array[Array[Int]]) -> Int = n4(rows)
def n4(rows: Array[Array[Int]]) -> Int = n5(rows)
def n5(rows: Array[Array[Int]]) -> Int = rows.length()
puts(n1([[1], [2, 3]]))

# The constraint still bites: a wider declaration doesn't loosen a
# narrower one already on the array, and a push of the wrong type fails.
def narrow(items: Array[Int]) -> Int = wide_union(items)
def wide_union(items: Array[Int | String]) -> Int
  begin
    items.push("text")
    0
  rescue error: TypeError
    1
  end
end
puts(narrow([1, 2]))
