attempts = 0
result = begin
  if attempts == 0
    attempts = 1
    raise "again"
  end
  42
rescue error: Int
  0
rescue error: String
  retry
end

puts(result)
