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

left = Box.new()
right = Box.new()
left.set(57)
right.set(58)
puts(left.value())
puts(right.value())
left.set(59)
puts(left.value())
puts(right.value())
