# deep_freeze reaches nested arrays, hash values, and instance fields,
# and terminates on a reference cycle.
class Node
  attr_accessor kids, name
  def initialize(name)
    @name = name
    @kids = []
  end
end

a = Node.new("a")
b = Node.new("b")
a.kids().push(b)
b.kids().push(a)
h = {"k": [1, [2, 3]], "n": a}
puts(h.frozen?())
h.deep_freeze()
puts([h.frozen?(), h["k"].frozen?(), h["k"][1].frozen?(), a.frozen?(), b.kids().frozen?()])
begin
  b.kids().push(1)
rescue error: FrozenError
  puts("frozen")
end
