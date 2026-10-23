module Greetable
  def greet() = "hi, #{self.name_for_greeting()}"
end

struct Point(x: Int, y: Int)
  include Greetable

  def name_for_greeting() = "point"
end

p1 = Point.new(1, 2)
puts(p1.greet())
