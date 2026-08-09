def empty[T]() -> Array[T] = []
result = empty[Int]()
begin
 result.push("wrong")
rescue error: TypeError
 42
end
