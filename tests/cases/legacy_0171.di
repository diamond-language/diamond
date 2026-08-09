def empty_scores() -> Hash[String, Int] = {}
def preserve[K, V](values: Hash[K, V]) -> Hash[K, V] = values
result = preserve(empty_scores())
begin
 result["wrong"] = "wrong"
rescue error: TypeError
 42
end
