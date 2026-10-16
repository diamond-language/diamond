begin
  1 << 2000000
rescue error: RangeError
  "caught"
end
