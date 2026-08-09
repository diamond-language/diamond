begin
 begin
  raise "text"
 rescue : TypeError
  0
 rescue : IndexError
  1
 end
rescue outer
 42
end
