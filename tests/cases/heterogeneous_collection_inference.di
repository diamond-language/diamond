def first_union(values: Array[Int | String]) -> Int | String
  values[0]
end

def hash_union(values: Hash[String | Symbol, Int | String]) -> Int | String | Nil
  values["answer"]
end

puts(first_union([42, "forty-two"]))
puts(hash_union({"answer": 42, :other: "other"}))
