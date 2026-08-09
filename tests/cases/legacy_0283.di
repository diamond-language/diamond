begin
 begin
  raise "failure"
 rescue error
  raise
 end
rescue outer
 42
end
