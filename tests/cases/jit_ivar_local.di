# JIT Phase 11 (docs/internal/jit-design.md): DIAMOND_OP_GET_IVAR joins
# DIAMOND_OP_NEW as a second class-producing terminal register_new_class_
# or_move_src recognizes, closing the roadmap's own named "an ivar load"
# INVOKE-receiver gap. `x = @box` compiles to the same GET_IVAR-into-a-
# temp-then-MOVE-into-x shape `x = Box.new(...)` already compiles to (NEW
# instead of GET_IVAR), confirmed via --dump-bytecode before writing this,
# so the existing MOVE-chasing loop covers this new terminal for free.
# `box: Box` must stay typed -- an untyped `initialize(box)` makes
# `@box = box`'s own value untraceable to a concrete class (correctly:
# an untyped parameter's runtime type genuinely isn't knowable here),
# permanently marking the field "unknown" via the exact same field_type_
# status/field_known_class this phase's own record_field_known_type now
# feeds -- confirmed directly (an earlier draft of this test used an
# untyped `box` and `run` stayed ineligible even with this phase's own
# jit.c change, for exactly that reason, not a bug). Before this phase,
# `x.double()` here made `run` bail whole (an unproven-type INVOKE
# receiver); all 4 functions now compile.
class Box
  def initialize(value)
    @value = value
  end
  def double() -> Int = @value * 2
end

class Holder
  def initialize(box: Box)
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
