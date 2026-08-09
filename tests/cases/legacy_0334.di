def run()
 result = []
 def collect(item)
  result.push(item + 1)
 end
 [1,2,3].each(collect)
 result
end
run()
