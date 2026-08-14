begin
  "abc".scan("not a regexp")
rescue error: TypeError
  42
end
