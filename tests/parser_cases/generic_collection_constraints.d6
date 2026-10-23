def pair[K, V](key: K, value: V) -> Hash[K, V] = {key: value}
result = pair("answer", 42)
begin
  result[1] = 2
  puts("no error")
rescue error: TypeError
  puts("blocked")
end

def empty[T]() -> Array[T] = []
list = empty[String]()
begin
  list.push(42)
  puts("no error")
rescue error: TypeError
  puts("blocked")
end
nil
