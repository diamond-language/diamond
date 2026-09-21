class Widget
  def initialize(n)
    @n = n
  end
  def double() -> Int = @n * 2
end

class Factory
  def make(box: Widget) -> Widget = box
end

class Holder
  def initialize(factory: Factory, box: Widget)
    @factory = factory
    @box = box
  end
  def replace_factory(value)
    @factory = value
  end
  def run(n) -> Int
    widget = @factory.make(@box)
    total = 0
    i = 0
    while i < n
      total = total + widget.double()
      i = i + 1
    end
    total
  end
end

puts(Holder.new(Factory.new(), Widget.new(21)).run(50000))
