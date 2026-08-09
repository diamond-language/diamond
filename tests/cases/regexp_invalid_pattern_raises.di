begin
  Regexp.new("(unclosed")
rescue error: RegexpError
  42
end
