# Regression test for this phase's own record_field_known_type fix: a
# field whose *only* SET_IVAR site is an attr_accessor-generated writer
# (never a bare `@field =`/`self.field =`) used to leave DiamondClass.
# field_type_status at 0 ("never assigned") forever, since compile_
# attribute_named's own writer-generation bypassed field_type_status/
# field_known_class entirely before this phase -- silently leaving
# DiamondFunction.ivar_known_class UINT8_MAX ("unknown") even though the
# writer's own declared parameter type (`box: Box` below) makes the real
# answer provably knowable. `mybox=` here is that generated writer, the
# *only* place `@mybox` is ever set.
class Box
  def initialize(value)
    @value = value
  end
  def double() -> Int = @value * 2
end

class Holder
  attr_accessor mybox: Box
  def run(n) -> Int
    x = @mybox
    total = 0
    i = 0
    while i < n
      total = total + x.double()
      i = i + 1
    end
    total
  end
end

h = Holder.new()
h.mybox=(Box.new(21))
puts(h.run(50000))
