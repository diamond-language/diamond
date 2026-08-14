begin
  JSON.parse("[1, 2")
rescue error: JSONError
  42
end
