def echo(value: String)
  puts(value)
end

begin
  raise "diamond"
rescue error: Int
  puts("integer")
rescue error: String
  echo(error)
end
