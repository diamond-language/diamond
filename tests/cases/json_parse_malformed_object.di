begin
  JSON.parse("{bad}")
rescue error: JSONError
  42
end
