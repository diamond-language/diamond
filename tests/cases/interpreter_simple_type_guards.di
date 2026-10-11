require "./jit_simple_type_guards"

def constrained_hash(value: Hash[String, Int]|Hash) -> Hash[String, Int]|Hash = value

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
    i += 1
  end
  run() + i
end
hash_guards()
