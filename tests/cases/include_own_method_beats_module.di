# A class's own methods beat an included module's, whichever comes first in
# the source. (Before, an own method defined BEFORE the include was shadowed
# by the module's, because lookup takes the last matching entry.)
module Greeter
  def hello() = "module hello"
  def shared() = "module shared"
end

module Loud
  def shared() = "loud shared"
end

class OwnFirst
  include Greeter
  def hello() = "own hello (after include)"
end

class OwnBefore
  def hello() = "own hello (before include)"
  include Greeter
end

class BothOrders
  def shared() = "own shared"
  include Greeter
  include Loud
end

class Plain
  include Greeter
end

# Two modules with the same method: the later include wins, as in Ruby.
class TwoModules
  include Greeter
  include Loud
end

puts(OwnFirst.new().hello())
puts(OwnBefore.new().hello())
puts(OwnBefore.new().shared())
puts(BothOrders.new().shared())
puts(BothOrders.new().hello())
puts(Plain.new().hello())
puts(TwoModules.new().shared())

# A module's own method beats one it includes, in either order.
module Base
  def name() = "base"
  def extra() = "base extra"
end
module Derived
  def name() = "derived (before include)"
  include Base
end
module Derived2
  include Base
  def name() = "derived2 (after include)"
end
class UsesDerived
  include Derived
end
class UsesDerived2
  include Derived2
end
puts(UsesDerived.new().name())
puts(UsesDerived.new().extra())
puts(UsesDerived2.new().name())

# The motivating case: == before include Comparable.
class Build
  def initialize(n: Int)
    @n = n
  end
  def n() = @n
  def ==(other)
    other is Build && @n == other.n()
  end
  include Comparable
  def <=>(other: Build) = @n <=> other.n()
end
puts(Build.new(5) == nil)
puts(Build.new(5) == Build.new(5))
puts(Build.new(5) > Build.new(4))

# A struct's generated == is a default, not an "own" method: an included
# Comparable's derived == still overrides it (compares by <=> only).
struct Score(points: Int, note: String)
  include Comparable
  def <=>(other: Score) = @points <=> other.points()
end
puts(Score.new(1, "a") == Score.new(1, "b"))
# ...while a hand-written == in the struct body wins, even before the include.
struct Exact(points: Int, note: String)
  def ==(other)
    other is Exact && @points == other.points() && @note == other.note()
  end
  include Comparable
  def <=>(other: Exact) = @points <=> other.points()
end
puts(Exact.new(1, "a") == Exact.new(1, "b"))
puts(Exact.new(1, "a") == nil)
nil
