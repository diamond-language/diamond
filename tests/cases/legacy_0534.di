attempts = 0
begin
 attempts = attempts + 1
 if attempts < 3
  raise "again"
 end
 attempts
rescue error
 retry
end
