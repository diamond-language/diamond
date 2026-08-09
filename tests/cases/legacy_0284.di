begin
 begin
  1 / 0
 rescue error: ZeroDivisionError
  raise
 end
rescue outer: ZeroDivisionError
 42
end
