module BaseState
  def set(value)
    @value = value
  end

  def value()
    @value
  end
end

module CombinedState
  include BaseState
end

class Box
  include CombinedState
end

box = Box.new()
box.set(60)
puts(box.value())
