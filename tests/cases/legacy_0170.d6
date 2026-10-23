def empty_ints() -> Array[Int] = []
def preserve[T](values: Array[T]) -> Array[T] = values
result = preserve(empty_ints())
begin
 result.push("wrong")
rescue error: TypeError
 42
end
