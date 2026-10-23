def run()
 def stringify(value: Int) -> String = "#{value}"
 result = array_map_typed([1, 2], stringify)
 begin
  result.push(3)
 rescue error: TypeError
  result
 end
end
run()
