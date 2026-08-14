begin
  re = Regexp.new("a")
  "abc".gsub(re)
rescue error: ArgumentError
  42
end
