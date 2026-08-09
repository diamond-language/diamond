def run()
 values = {}
 index = 0
 while index < 100
  values[index] = index
  index = index + 1
 end
 total = 0
 index = 0
 while index < 100
  total = total + values[index]
  index = index + 1
 end
 total
end
run()
