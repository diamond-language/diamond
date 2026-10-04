class Point
  def initialize(x: Int, y: Int)
    @x = x
    @y = y
  end
end
class Named
  def initialize(n: String)
    @n = n
  end
  def inspect() = "Named<#{@n}>"
end
class Node
  def initialize(v: Int)
    @v = v
    @next = nil
  end
  def link(other: Node)
    @next = other
  end
end
puts("a\"b\\c\nd\t".inspect())
puts(["a", "b, c", 1, 2.5, nil, true, :sym].inspect())
puts({"k": [1, "v"], "z": {"n": nil}}.inspect())
puts(:sym.inspect())
puts(nil.inspect())
puts(1.inspect())
puts(Point.new(1, 2).inspect())
puts([Point.new(3, 4), Named.new("x")].inspect())
puts(Named.new("y").inspect())
a = Node.new(1)
b = Node.new(2)
a.link(b)
b.link(a)
puts(a.inspect())
puts("é\u0001".inspect())

# to_s is unchanged: no quotes, no colon.
puts(["a", :b].to_s())

# A user-defined inspect receives a zero-argument call and must return a String.
class BadInspect
  def inspect() = 42
end
begin
  [BadInspect.new()].inspect()
rescue error: TypeError
  puts("rescued: #{error.message()}")
end

# Called with arguments: arity error.
begin
  [1].inspect(2)
rescue error: StandardError
  puts("arity rejected")
end
