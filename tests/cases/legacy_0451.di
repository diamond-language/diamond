def run()
 values = {}
 index = 0
 while index < 1000
  values[index] = index * 2
  index = index + 1
 end
 ok = true
 index = 0
 while index < 1000
  if values.key_at(index) != index || values.value_at(index) != index * 2
   ok = false
  end
  index = index + 1
 end
 "#{ok}, #{values.length()}"
end
run()
