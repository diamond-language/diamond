class Counter
  def self.zero()
    0
  end
end
puts(Counter.zero())

class Widget
  def initialize(x)
    @x = x
  end
  def show()
    @x
  end
  def self.make(x = 99)
    Widget.new(x)
  end
end
puts(Widget.make().show())
puts(Widget.make(5).show())
w = Widget.new(1)
puts(w.show())

module Named
  def self.hi(name)
    name + "!"
  end
end
puts(Named.hi("Ada"))
