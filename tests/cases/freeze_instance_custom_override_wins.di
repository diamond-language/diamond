class Widget
  def initialize()
    @locked = false
  end
  def freeze()
    @locked = true
    "custom freeze"
  end
  def frozen?()
    @locked
  end
end
w = Widget.new()
puts(w.freeze())
puts(w.frozen?())
