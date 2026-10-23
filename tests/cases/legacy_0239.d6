module Types
 module_function
 def empty[T]() -> Array[T] = []
end
result=Types.empty[Int]()
begin
 result.push("wrong")
rescue error: TypeError
 42
end
