module Types
 def empty[T]() -> Array[T] = []
 module_function empty
end
result=Types.empty[String]()
begin
 result.push(42)
rescue error: TypeError
 42
end
