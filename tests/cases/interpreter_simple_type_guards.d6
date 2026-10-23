require "../support/simple_type_guards"

def constrained_hash(value: Hash[String, Int]|Hash) -> Hash[String, Int]|Hash = value

def plain_array_first(value: Array|Array[Int]) -> Array|Array[Int] = value
def plain_hash_first(value: Hash|Hash[String, Int]) -> Hash|Hash[String, Int] = value

def hash_guards()
  i = 0
  while i < 20
    values = constrained_hash({"count": i})
    rejected = 0
    begin
      values["count"] = "bad"
    rescue error: TypeError
      rejected += 1
    end
    begin
      values[1] = i
    rescue error: TypeError
      rejected += 1
    end
    unless rejected == 2 && values["count"] == i
      raise RuntimeError.new("hash union lost key/value constraints")
    end
    mixed = constrained_hash({"count": "allowed"})
    mixed[1] = false
    unless mixed[1] == false
      raise RuntimeError.new("plain hash fallback attached constraints")
    end
    plain_array = plain_array_first([i])
    plain_array.push("allowed")
    plain_hash = plain_hash_first({"count": i})
    plain_hash[1] = false
    unless plain_array[1] == "allowed" && plain_hash[1] == false
      raise RuntimeError.new("plain-first union attached constraints")
    end
    i += 1
  end
  run() + i
end
hash_guards()
