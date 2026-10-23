interface Consumer
 def accept(value)
end
class Typed
 def accept(value: Int)
  value
 end
end
class Dynamic
 def accept(value)
  value
 end
end
[Typed.new() is Consumer, Dynamic.new() is Consumer]
