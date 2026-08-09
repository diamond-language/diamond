def identity[T](value: T) -> T = value
begin
 identity[Int]("wrong")
rescue error: TypeError
 42
end
