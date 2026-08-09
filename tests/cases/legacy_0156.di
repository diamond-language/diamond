class Box
 def wrap[T](value: T) -> Array[T] = [value]
end
result = Box.new().wrap("diamond")
begin
 result.push(42)
rescue error: TypeError
 result
end
