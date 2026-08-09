def run()
 def wrong(value) -> Int
  value
 end
 begin
  array_map_string([], wrong)
 rescue error: TypeError
  42
 end
end
run()
