def run()
 def add(acc, x)
  acc + x
 end
 enumerable_reduce({"a":1,"b":2,"c":3}, 0, add)
end
run()
