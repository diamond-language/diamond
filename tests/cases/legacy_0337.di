def run()
 result = []
 def collect(k, v)
  result.push(v)
 end
 values = {"a":1,"b":2}
 hash_each(values, collect)
 result
end
run()
