# A nested def can call itself (directly or from a block in its body); one
# that doesn't still captures nothing, so it can go to Thread.new.
def factorial(n)
  def fact(k) = if k <= 1 then 1 else k * fact(k - 1) end
  fact(n)
end
def names(tree)
  seen = []
  def visit(node, seen)
    seen.push(node["name"])
    node["kids"].each() do |kid| visit(kid, seen) end
  end
  visit(tree, seen)
  seen
end
def factory()
  def greet() = "hi"
  greet
end
tree = {"name": "a", "kids": [{"name": "b", "kids": []}, {"name": "c", "kids": [{"name": "d", "kids": []}]}]}
[factorial(5), names(tree), Thread.new(factory()).join()]
