def run()
 result = []
 def collect(item)
  result.push(item + 1)
 end
 values = [1,2,3]
 array_each(values, collect)
 result
end
run()
