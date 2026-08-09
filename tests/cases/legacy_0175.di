def empty_pair[K, V]() -> Hash[K, V] = {}
result = empty_pair[String, Int]()
result["answer"] = 42
result["answer"]
