module Outer
 module_function
 def outer() = 40
 module Inner
  def value() = 2
 end
end
class Box
 include Outer::Inner
end
Outer.outer() + Box.new().value()
