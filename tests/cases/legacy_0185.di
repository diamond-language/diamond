module Collections
 def empty[T]() -> Array[T] = []
end
class Factory
 include Collections
end
result = Factory.new().empty[String]()
begin
 result.push(42)
rescue error: TypeError
 42
end
