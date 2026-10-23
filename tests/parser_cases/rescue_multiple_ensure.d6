result = begin
  raise "diamond"
rescue error: Int
  0
rescue error: String
  42
ensure
  puts("ensure")
end

puts(result)
