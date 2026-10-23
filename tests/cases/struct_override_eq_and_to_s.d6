# A struct's hand-written == or to_s replaces the generated one, while the
# other generated members are untouched. Here two Points are equal when their
# coordinates are, ignoring the label; to_s is custom. Tag still gets the
# generated versions, and a plain == against nil no longer needs a workaround.
struct Point(x: Int, y: Int, label: String)
  def ==(other)
    other is Point && @x == other.x() && @y == other.y()
  end

  def to_s() -> String = "(#{@x}, #{@y})"
end

struct Tag(name: String)
end

a = Point.new(1, 2, "first")
b = Point.new(1, 2, "second")
c = Point.new(9, 2, "first")
puts(a == b)
puts(a == c)
puts(a != c)
puts(a == nil)
puts(a == 5)
puts(a.to_s())
puts("#{c}")
puts(a.label())
puts(Tag.new("x") == Tag.new("x"))
puts(Tag.new("x") == Tag.new("y"))
puts(Tag.new("x").to_s())
puts([a, c].include?(b))
puts(Point.new(3, 4, "z").x())
nil
