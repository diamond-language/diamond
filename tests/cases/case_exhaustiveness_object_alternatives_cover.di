# Comma-joined object-pattern alternatives each cover their own class when
# every reader is a bare binding or `_` (an empty `Class{}` counts): any of
# them matching selects the same branch, so together they cover Circle and
# Square with no `else`. Also works over a sealed hierarchy.
class Circle
  def initialize(r: Int)
    @r = r
  end
  def r() = @r
end
class Square
  def initialize(s: Int)
    @s = s
  end
  def s() = @s
end
def kind(shape: Circle | Square)
  case shape
  when Circle{}, Square{}
    "some shape"
  end
end

sealed class Event
end
class Coin < Event
  def initialize(cents: Int)
    @cents = cents
  end
  def cents() = @cents
end
class Select < Event
end
class Refund < Event
end
def handle(event: Event)
  case event
  when Coin{cents: _}, Select{}
    "money or pick"
  when Refund{}
    "refund"
  end
end

puts(kind(Circle.new(1)))
puts(kind(Square.new(2)))
puts(handle(Coin.new(25)))
puts(handle(Select.new()))
puts(handle(Refund.new()))
nil
