module Base
 def value() = 1
end
module Combined
 include Base
 def value() = 2
end
class Box
 include Combined
end
Box.new().value()
