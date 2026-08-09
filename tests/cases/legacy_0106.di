def run()
 def stringify(value) -> String
  "ok"
 end
 result = array_map_string([1], stringify)
 begin
  result.push(42)
 rescue error: TypeError
  42
 end
end
run()
