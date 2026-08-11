begin
  begin
    raise "diamond"
  rescue error: Int
    0
  rescue error: String
    raise
  end
rescue outer: String
  puts(outer)
end
