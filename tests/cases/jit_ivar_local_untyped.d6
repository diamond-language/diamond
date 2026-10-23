# A field whose only assignment is an untyped parameter's value can't be
# proven to hold a single concrete class -- correctly stays ineligible.
# `box` (no type annotation) has no compile-time-known type at all, so
# `@box = box` correctly marks DiamondClass.field_type_status "unknown"
# (2) rather than wrongly trusting whatever class happened to be passed
# at the one call site this file has. `run` never becomes eligible;
# `initialize`/`double` still compile independently (2, not 3).
class Box
  def initialize(value)
    @value = value
  end
  def double() -> Int = @value * 2
end

class Holder
  def initialize(box)
    @box = box
  end
  def run(n) -> Int
    x = @box
    total = 0
    i = 0
    while i < n
      total = total + x.double()
      i = i + 1
    end
    total
  end
end

puts(Holder.new(Box.new(21)).run(50000))
