class Circle
  def initialize(r: Int)
    @r = r
  end
end
def describe(shape: Circle | Nil)
  case shape
  when Circle
    "circle"
  when nil
    "nothing"
  end
end
puts(describe(nil))
puts(describe(Circle.new(1)))
