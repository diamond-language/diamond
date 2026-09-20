# The outer call's declared return type (`-> Widget`) only proves "an
# instance of Widget or a compatible subclass" -- the *inner* call
# (`x.double()`) still dispatches through diamond_jit_invoke_instance's
# own real lookup_method-gated logic, so a subclass instance actually
# constructed (SuperWidget, overriding double()) is dispatched correctly,
# not devirtualized to Widget's own declared method. 84 (21*4 via the
# override), not 42.
class Widget
  def initialize(n)
    @n = n
  end
  def double() -> Int = @n * 2
end

class SuperWidget < Widget
  def double() -> Int = @n * 4
end

class Factory
  def make() -> Widget = SuperWidget.new(21)
end

def run() -> Int
  f = Factory.new()
  x = f.make()
  x.double()
end

puts(run())
