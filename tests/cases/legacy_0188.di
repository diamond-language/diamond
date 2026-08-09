module First
 def value() = 1
end
module Second
 def value() = 2
end
module Combined
 include First
 include Second
end
class Box
 include Combined
end
Box.new().value()
