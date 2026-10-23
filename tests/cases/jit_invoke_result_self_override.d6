# `go` is declared on Factory (self's own known_types entry there is
# Factory's class id), but called here on a SuperFactory instance --
# self.make() must dispatch to SuperFactory's own override (returning a
# SuperWidget, not a Widget), and the outer x.double() call must then
# dispatch through SuperWidget's own override too, via diamond_jit_
# invoke_instance's real lookup_method-gated logic at every step, not
# devirtualized to whatever Factory/Widget's own declared methods would
# do. 84 (21*4 via both overrides), not 42.
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
  def make() -> Widget = Widget.new(21)
  def go() -> Int
    x = self.make()
    x.double()
  end
end

class SuperFactory < Factory
  def make() -> Widget = SuperWidget.new(21)
end

puts(SuperFactory.new().go())
