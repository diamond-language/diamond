begin
  raise "diamond"
rescue error
  error
rescue error: String
  error
end
