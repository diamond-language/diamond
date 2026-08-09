attempts=0
begin
 attempts=attempts+1
 if attempts==1
  [1][4]
 end
rescue : TypeError
 0
rescue : IndexError
 retry
else
 42
end
