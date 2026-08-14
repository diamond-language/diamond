begin
  re = Regexp.new("a")
  "abc".gsub(re, 5)
rescue error: TypeError
  42
end
