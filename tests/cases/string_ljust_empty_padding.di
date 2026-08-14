begin
  "abc".ljust(6, "")
rescue error: ArgumentError
  42
end
