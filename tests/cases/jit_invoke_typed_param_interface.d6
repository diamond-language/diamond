# An interface-typed parameter's type_set member id lands in
# [DIAMOND_TYPE_INTERFACE_BASE, ...), outside parameter_is_single_class's
# [DIAMOND_TYPE_CLASS_BASE, DIAMOND_TYPE_VARIABLE_BASE) range check, so it's
# correctly rejected -- an interface has no single concrete runtime class
# by construction (Box and OtherDoubler both satisfy Doubler here, so no
# compile-time proof of "always an Instance of one specific class" could
# ever be sound for this receiver). `run_loop` never compiles;
# `Box#double`/`Box#initialize` still do (2, not 3).
interface Doubler
  def double()
end

class Box
  def initialize(value)
    @value = value
  end
  def double() -> Int = @value * 2
end

def run_loop(box: Doubler, n) -> Int
  total = 0
  i = 0
  while i < n
    total = total + box.double()
    i = i + 1
  end
  total
end

b = Box.new(21)
puts(run_loop(b, 50000))
