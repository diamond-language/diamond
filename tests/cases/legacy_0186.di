module Base
 def value() = 1
end
module Combined
 include Base
 def other() = 41
end
class Box
 include Combined
end
Box.new().value() + Box.new().other()
