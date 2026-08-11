result = begin
  7
rescue error: Int
  1
rescue error: String
  2
else
  42
end

puts(result)
