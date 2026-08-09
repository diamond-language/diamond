begin
 begin
  [1][4]
 rescue : TypeError
  0
 rescue error: IndexError
  raise
 end
rescue outer: IndexError
 42
end
