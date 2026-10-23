def empty_like[T](sample: T) -> Array[T] = []
def preserve[T](values: Array[T]) -> Array[T] = values
result = preserve(empty_like(["diamond"]))
begin
 result.push([42])
rescue error: TypeError
 42
end
