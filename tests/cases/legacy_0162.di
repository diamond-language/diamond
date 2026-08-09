def singleton[T](value: T) -> Array[T] = [value]
result = singleton({"items": [1]})
begin
 result.push({"items": ["wrong"]})
rescue error: TypeError
 42
end
