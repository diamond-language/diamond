def pair[K, V](key: K, value: V) -> Hash[K, V] = {key: value}
result = pair("answer", 42)
begin
 result[1] = 2
rescue error: TypeError
 result["answer"]
end
