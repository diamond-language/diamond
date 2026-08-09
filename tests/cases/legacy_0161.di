def run()
 def words(value: Int) -> Array[String] = ["#{value}"]
 result = array_map_typed([], words)
 begin
  result.push([42])
 rescue error: TypeError
 42
 end
end
run()
