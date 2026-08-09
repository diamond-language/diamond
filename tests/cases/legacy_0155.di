def pair[K, V](key: K, value: V) -> Hash[K, V] = {key: value}
result = pair("answer", 42)
begin
 result["other"] = "wrong"
rescue error: TypeError
 result["answer"]
end
