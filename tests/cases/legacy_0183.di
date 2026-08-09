module Values
 def value() = 1
end
class Box
 include Values
 def value() = 3
end
Box.new().value()
