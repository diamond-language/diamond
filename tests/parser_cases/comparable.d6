puts(1 <=> 2)
puts(2 <=> 1)
puts(1 <=> 1)
puts(2.5 <=> 1)
puts((0.0 / 0.0) <=> 1)

class Box
  include Comparable
  def initialize(size)
    @size = size
  end
  def <=>(other)
    @size - other.size()
  end
  def size() = @size
end

puts(Box.new(1) < Box.new(2))
puts(Box.new(2) < Box.new(1))
puts(Box.new(1) <= Box.new(1))
puts(Box.new(2) > Box.new(1))
puts(Box.new(1) >= Box.new(2))
puts(Box.new(3) == Box.new(3))
puts(Box.new(5).between?(Box.new(1), Box.new(10)))
puts(Box.new(15).clamp(Box.new(1), Box.new(10)).size())

# Precedence: `<=>` binds looser than `<`/`<=`, so this reads as
# `(1 < 2) <=> (3 < 4)` -- two Bools, which `<=>` isn't defined for
# (Bool doesn't support it any more than Ruby's own TrueClass does),
# hence Nil rather than a raised error. The precedence grouping itself
# is what's under test here, not the Bool operands' own comparability.
puts((1 < 2 <=> 3 < 4))
nil
