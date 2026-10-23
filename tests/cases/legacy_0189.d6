module Base
 def empty[T]() -> Array[T] = []
end
module Combined
 include Base
end
class Factory
 include Combined
end
result = Factory.new().empty[Int]()
begin
 result.push("wrong")
rescue error: TypeError
 42
end
