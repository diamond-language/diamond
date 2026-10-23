class Factory
 def empty[T]() -> Array[T] = []
end
result = Factory.new().empty[String]()
begin
 result.push(42)
rescue error: TypeError
 42
end
