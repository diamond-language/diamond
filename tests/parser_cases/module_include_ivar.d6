module State
  def set(value)
    @value = value
  end

  def value()
    @value
  end
end

class Box
  include State
end

box = Box.new()
box.set(56)
puts(box.value())
