module First
 def value() = 1
end
module Second
 def value() = 2
end
class Box
 include First
 include Second
end
Box.new().value()
