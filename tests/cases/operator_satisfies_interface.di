interface Addable
 def +(other)
end
class Box
 def initialize(x)
  @x = x
 end
 def +(other) = Box.new(@x)
end
Box.new(1) is Addable
