# Inside `def self.x`, a bare `name(...)` is `self.name(...)`: it reaches a
# singleton method defined above, below, or in a superclass, dispatches on
# self's actual class, and takes arguments and a block.
class Greeter
  def self.greet(name) = prefix() + name + suffix()
  def self.prefix() = "hello "
  def self.suffix() = "!"
  def self.twice(n) = n * 2
  def self.sum_doubled(list) = list.map() do |n|
    twice(n)
  end
  def self.kind() = tag()
  def self.tag() = "base"
end
class Shouter < Greeter
  def self.prefix() = "HEY "
  def self.tag() = "shout"
  def self.loud(name) = greet(name)
end
puts(Greeter.greet("a"))
puts(Shouter.greet("b"))
puts(Shouter.loud("c"))
puts(Greeter.sum_doubled([1, 2, 3]).to_s())
puts(Greeter.kind())
puts(Shouter.kind())
