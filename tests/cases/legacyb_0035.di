module State
 def value() = @value
end
class Box
 include State
end
Box.new().value()
