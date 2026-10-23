module State
 def set_value(value)
  @value = value
 end
 def value() = @value
end
module Combined
 include State
end
class Box
 include Combined
end
box = Box.new()
box.set_value(["diamond"])
box.value()
