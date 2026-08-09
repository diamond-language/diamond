module Outer
 VALUE = 40
 module Inner
  OFFSET = 2
  def total() = VALUE + OFFSET
 end
 class Box
  include Inner
 end
end
Outer::Box.new().total()
