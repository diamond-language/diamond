def twice(value: Int)
  value + value
end

value = begin
  21
rescue error: String
  21
rescue error: Bool
  21
end

puts(twice(value))
