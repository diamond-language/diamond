module State
 def module_value() = @value
end
class Box
 include State
 def set_value(value)
  @value = value
 end
end
box = Box.new()
box.set_value(42)
box.module_value()
