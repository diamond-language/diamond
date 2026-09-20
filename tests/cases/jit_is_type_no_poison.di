# JIT Phase 14: an unrelated DIAMOND_OP_IS_TYPE elsewhere in a function
# body used to poison parameter_never_reassigned/register_new_class_or_
# move_src for EVERY register in that function, not just the narrowed
# one -- both scans bail their whole walk the moment they hit any
# instruction outside their own hand-maintained opcode whitelist,
# regardless of which register they're actually tracing. Before this
# phase, `run` below -- whose only real receiver-chaining work is the
# ordinary Phase 13 `self.make().double()` shape, unrelated to the `is`
# check -- would NOT compile, solely because of the `is` check's mere
# presence in the same function. Confirmed via a stash-based A/B: 2
# compiled functions (make, double) before this phase's fix, 3
# (make, double, run) after.
class Widget
  def double() -> Int
    21
  end
end

class Factory
  def make() -> Widget
    Widget.new()
  end
  def run(x: Int | String) -> Int
    unrelated = 0
    if x is Int
      unrelated = 1
    end
    y = self.make()
    y.double() + unrelated
  end
end

f = Factory.new()
total = 0
i = 0
while i < 5
  total = total + f.run(1)
  i = i + 1
end
puts(total)
