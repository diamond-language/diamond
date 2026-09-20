# A `redefine_method` call anywhere in the whole program -- even on a
# completely unrelated class, never touching Factory#make -- disables
# DiamondFunction.register_known_class trust everywhere (Diamond
# Function.redefine_method_used_anywhere is a whole-program flag, not a
# per-method one; see that field's own comment, src/vm.h, for why this
# phase deliberately doesn't try to track a finer-grained answer). `run`
# correctly never becomes eligible, even though nothing it calls is
# anywhere near the redefinition.
class Widget
  def initialize(n)
    @n = n
  end
  def double() -> Int = @n * 2
end

class Factory
  def make() -> Widget = Widget.new(21)
end

class Unrelated
  def greet() = "hello"
  def self.replacement_patch()
    def greet_replacement() = "goodbye"
    greet_replacement
  end
end

Unrelated.redefine_method("greet", Unrelated.replacement_patch())

def run(f: Factory, n) -> Int
  x = f.make()
  total = 0
  i = 0
  while i < n
    total = total + x.double()
    i = i + 1
  end
  total
end

puts(run(Factory.new(), 50000))
