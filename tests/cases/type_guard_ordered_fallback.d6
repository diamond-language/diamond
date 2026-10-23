def ordered_array(value: Int | Array[Int] | Array) -> Int | Array[Int] | Array = value
def ordered_hash(value: Nil | Hash[String, Int] | Hash) -> Nil | Hash[String, Int] | Hash = value
def plain_first(value: Array | Array[Int] | Int) -> Array | Array[Int] | Int = value
def forward_array(value) = ordered_array(value)

def verify_ordered_guards()
  count = 0
  i = 0
  while i < 20
    big = 9223372036854775807 + 1
    unless ordered_array(big) == big && ordered_hash(nil) == nil
      raise RuntimeError.new("plain union prefix did not match")
    end
    array = ordered_array([i])
    begin
      array.push("rejected")
      raise RuntimeError.new("generic array constraint was lost")
    rescue error: TypeError
      count += 1
    end
    fallback = ordered_array([false])
    fallback.push("allowed")
    unconstrained = plain_first([i])
    unconstrained.push(false)
    hash = ordered_hash({"count": i})
    begin
      hash["count"] = false
      raise RuntimeError.new("generic hash constraint was lost")
    rescue error: TypeError
      count += 1
    end
    fallback_hash = ordered_hash({"count": false})
    fallback_hash[1] = "allowed"
    begin
      forward_array("rejected")
      raise RuntimeError.new("invalid union value was accepted")
    rescue error: TypeError
      count += 1
    end
    i += 1
  end
  count
end
verify_ordered_guards()
