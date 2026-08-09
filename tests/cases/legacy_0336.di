def run()
 result = []
 def collect(k, v)
  result.push(v)
 end
 {"a":1,"b":2}.each(collect)
 result
end
run()
