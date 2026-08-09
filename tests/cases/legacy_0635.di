begin
 begin
  raise "boom"
 rescue error
  raise if true
 end
rescue error
 error
end
