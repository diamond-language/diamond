begin
  raise true
rescue error: Int | String
  puts("first")
rescue error: Bool | Nil
  puts(error)
end
