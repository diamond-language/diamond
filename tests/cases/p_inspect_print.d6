# p(value) prints value.inspect() and evaluates to value.
p(5)
p("a\nb")
p(nil)
p(:sym)
p([1, "two", nil, 2.5])
p({"k": [1]})

# It returns its argument, so it can wrap an expression.
kept = p([1, 2, 3])
puts(kept.length())
puts(p(2) + p(3))

class Point
  attr_reader x: Int
  def initialize(x: Int)
    @x = x
  end
  def show() = p(self)
  def self.make() = p(Point.new(9))
end
Point.new(3).show()
Point.make()

# A local named p, a method named p and a function named p all keep their meaning.
p = 10
puts(p)

class Printer
  def p(value) = puts("method p #{value}")
  def run() = p(7)
end
Printer.new().run()

class Doubler
  def self.p(value) = value * 2
  def self.run() = p(4)
end
puts(Doubler.run())
