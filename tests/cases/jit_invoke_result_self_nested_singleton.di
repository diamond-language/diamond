# The redefine_method patch-factory idiom's own def, nested directly
# inside a singleton method, behaves like a genuine instance method
# (nested_in_singleton_method in compile_definition) -- Phase 13's own
# fix covers this shape too, not just an ordinary `def`. Installed via
# `define_method` rather than `redefine_method` specifically so this
# test doesn't itself trip DiamondFunction.redefine_method_used_
# anywhere (a whole-program flag -- see that field's own comment,
# src/vm.h) and mask the very thing being tested.
class Widget
  def initialize(n)
    @n = n
  end
  def double() -> Int = @n * 2
end

class Factory
  def make() -> Widget = Widget.new(21)
  def self.patch()
    def installed_run(n) -> Int
      x = self.make()
      total = 0
      i = 0
      while i < n
        total = total + x.double()
        i = i + 1
      end
      total
    end
    installed_run
  end
end

Factory.define_method("installed_run", Factory.patch())
puts(Factory.new().installed_run(50000))
